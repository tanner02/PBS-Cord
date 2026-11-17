#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Cache public Store artwork via anonymous FTP without uploading local icons."""
import argparse
import ftplib
import io
import json
import posixpath
import re
import sys
import urllib.parse
import urllib.request
import uuid
from html.parser import HTMLParser

META_LIMIT, PAGE_LIMIT, MANIFEST_LIMIT = 1024 * 1024, 2 * 1024 * 1024, 256 * 1024
TITLE = re.compile(r'(?:PPSA|CUSA)[0-9]{5}\Z')
CONTENT = re.compile(r'[A-Z]{2}[0-9]{4}-((?:PPSA|CUSA)[0-9]{5})_[0-9]{2}-[A-Za-z0-9_]{16}\Z')
DIGITS = re.compile(r'[0-9]{1,12}\Z')
LOCALES = {'EP': 'en-gb', 'HP': 'en-hk', 'JP': 'ja-jp', 'UP': 'en-us'}


def image_url(value):
    return (isinstance(value, str) and len(value) <= 256 and
            value.startswith(('https://image.api.playstation.com/', 'https://image.dl.playstation.net/')) and
            re.fullmatch(r'[A-Za-z0-9:/._?=&%+~\-]+', value) is not None)


def store_url(content='', concept=''):
    if DIGITS.fullmatch(concept):
        return 'https://store.playstation.com/en-us/concept/' + concept
    locale = LOCALES.get(content[:2])
    if CONTENT.fullmatch(content) and locale:
        return 'https://store.playstation.com/' + locale + '/product/' + content
    raise ValueError('No supported public Store identifier in game metadata')


def valid_store_url(value):
    if not isinstance(value, str):
        return False
    if re.fullmatch(r'https://store\.playstation\.com/en-us/concept/[0-9]{1,12}', value):
        return True
    match = re.fullmatch(r'https://store\.playstation\.com/([^/]+)/product/([^/]+)', value)
    return bool(match and CONTENT.fullmatch(match[2]) and LOCALES.get(match[2][:2]) == match[1])


def parse_json(data):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError('Duplicate JSON field')
            result[key] = value
        return result
    return json.loads(data, object_pairs_hook=unique)


def retrieve(ftp, path, limit):
    data = bytearray()
    def receive(block):
        if len(data) + len(block) > limit:
            raise ValueError('FTP file exceeds size limit')
        data.extend(block)
    try:
        ftp.retrbinary('RETR ' + path, receive)
    except ftplib.error_perm as error:
        if str(error).startswith('550'):
            return None
        raise
    return bytes(data)


class StoreRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, newurl):
        parsed = urllib.parse.urlsplit(newurl)
        if parsed.scheme != 'https' or parsed.hostname != 'store.playstation.com' or parsed.port not in (None, 443) or parsed.username or parsed.password:
            raise ValueError('Store redirected outside its public HTTPS origin')
        return super().redirect_request(request, fp, code, message, headers, newurl)


class StorePage(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=False)
        self.current, self.parts, self.documents = None, [], []

    def handle_starttag(self, tag, attributes):
        attrs = dict(attributes)
        if tag == 'script' and (attrs.get('type') == 'application/ld+json' or attrs.get('id') == '__NEXT_DATA__'):
            self.current, self.parts = attrs.get('id') == '__NEXT_DATA__', []

    def handle_data(self, data):
        if self.current is not None:
            self.parts.append(data)

    def handle_endtag(self, tag):
        if tag == 'script' and self.current is not None:
            try:
                self.documents.append((self.current, parse_json(''.join(self.parts))))
            except (ValueError, RecursionError):
                pass
            self.current, self.parts = None, []


def artwork(page, content, concept):
    parser = StorePage()
    parser.feed(page)
    concept_matches, products = False, []
    for is_next, value in parser.documents:
        if is_next and isinstance(value, dict):
            props = value.get('props', {})
            page_props = props.get('pageProps', {}) if isinstance(props, dict) else {}
            concept_matches = bool(isinstance(page_props, dict) and concept and page_props.get('conceptId') == concept)
        elif not is_next:
            products.extend(value if isinstance(value, list) else [value])
    for product in products:
        if (isinstance(product, dict) and product.get('@type') == 'Product' and
                (concept_matches or (content and product.get('sku') == content)) and image_url(product.get('image'))):
            return product['image']
    raise ValueError('No matching Product with an allowed public artwork URL')


def manifest(data):
    value = {'version': 1, 'images': {}} if data is None else parse_json(data)
    if (not isinstance(value, dict) or set(value) != {'version', 'images'} or type(value['version']) is not int or value['version'] != 1 or
            not isinstance(value['images'], dict) or len(value['images']) > 32 or
            any(not valid_store_url(key) or not image_url(image) for key, image in value['images'].items())):
        raise ValueError('Existing artwork manifest is invalid; refusing to replace it')
    return value


def install(ftp, directory, key, image):
    target = directory + '/game-artwork.json'
    old = retrieve(ftp, target, MANIFEST_LIMIT)
    value = manifest(old)
    if key not in value['images'] and len(value['images']) == 32:
        raise ValueError('Artwork manifest already contains 32 entries')
    value['images'][key] = image
    data = (json.dumps(value, sort_keys=True, separators=(',', ':')) + '\n').encode('utf-8')
    if len(data) > MANIFEST_LIMIT:
        raise ValueError('Artwork manifest exceeds size limit')
    parts = directory.split('/')
    for index in range(1, len(parts)):
        path = '/'.join(parts[:index + 1])
        try:
            ftp.mkd(path)
        except ftplib.error_perm as error:
            if not str(error).startswith('550'):
                raise
            ftp.cwd(path)  # Distinguish an existing directory from denied creation.
    unique = uuid.uuid4().hex
    temporary, backup = directory + '/.game-artwork-' + unique + '.tmp', target + '.' + unique + '.bak'
    staged, backed_up = False, False
    try:
        staged = True
        ftp.storbinary('STOR ' + temporary, io.BytesIO(data))
        if retrieve(ftp, temporary, MANIFEST_LIMIT) != data:
            raise ValueError('Staged artwork manifest verification failed')
        if retrieve(ftp, target, MANIFEST_LIMIT) != old:
            raise ValueError('Artwork manifest changed during update; refusing to overwrite it')
        if old is not None:
            ftp.rename(target, backup)
            backed_up = True
        try:
            ftp.rename(temporary, target)
            staged = False
        except Exception:
            if backed_up:
                ftp.rename(backup, target)
            raise
    finally:
        if staged:
            try:
                ftp.delete(temporary)  # Never delete anything except our unique temp.
            except ftplib.error_reply as error:
                if not str(error).startswith('226'):  # This server uses 226 for successful DELE.
                    raise
            except ftplib.all_errors:
                pass


def main():
    args = argparse.ArgumentParser(description=__doc__)
    args.add_argument('--host', required=True)
    args.add_argument('--port', type=int, default=2121)
    args.add_argument('--title', required=True)
    args.add_argument('--output-dir', default='/data/pbs-discord')
    options = args.parse_args()
    directory = options.output_dir.rstrip('/')
    if not TITLE.fullmatch(options.title) or not 1 <= options.port <= 65535:
        raise ValueError('Expected PPSA/CUSA plus five digits and a valid FTP port')
    if not directory.startswith('/') or directory != posixpath.normpath(directory) or any(ord(c) < 32 or c == '\\' for c in directory):
        raise ValueError('Output directory must be an absolute normalized FTP path')
    with ftplib.FTP(timeout=10) as ftp:
        ftp.connect(options.host, options.port)
        ftp.login()
        raw = retrieve(ftp, '/user/appmeta/' + options.title + '/param.json', META_LIMIT)
        metadata = parse_json(raw) if raw is not None else None
        if not isinstance(metadata, dict) or metadata.get('titleId') != options.title:
            raise ValueError('Matching param.json is missing or invalid')
        content, concept = metadata.get('contentId', ''), metadata.get('conceptId', '')
        match = CONTENT.fullmatch(content) if isinstance(content, str) else None
        if not isinstance(content, str) or (content and (not match or match[1] != options.title)):
            raise ValueError('Invalid or mismatched game content ID')
        if not isinstance(concept, str) or (concept and not DIGITS.fullmatch(concept)):
            raise ValueError('Invalid game concept ID')
        key = store_url(content, concept)
        request = urllib.request.Request(key, headers={'User-Agent': 'PBS-Discord-Artwork/1', 'Accept': 'text/html', 'Accept-Encoding': 'identity'})
        with urllib.request.build_opener(StoreRedirect()).open(request, timeout=10) as response:
            page = response.read(PAGE_LIMIT + 1)
        if len(page) > PAGE_LIMIT:
            raise ValueError('Store page exceeds size limit')
        image = artwork(page.decode('utf-8'), content, concept)
        install(ftp, directory, key, image)
    print(options.title)
    print(image)


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print('error: ' + str(error).replace('\r', ' ').replace('\n', ' ')[:240], file=sys.stderr)
        sys.exit(1)
