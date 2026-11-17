#!/usr/bin/env python3
"""Decode media recovered by an independent peer after the real sender path."""
from pathlib import Path
import json, subprocess, sys, math, array
args=sys.argv[1:]
executable,fixture,output,ffmpeg,ffprobe=args[:5]
mode=args[5] if len(args)>5 else None
directory=Path(output)
directory.mkdir(parents=True,exist_ok=True)
subprocess.run([executable,fixture,output]+([mode] if mode else []),check=True,timeout=35)
subprocess.run([ffmpeg,'-hide_banner','-loglevel','error','-xerror','-i',str(directory/'recovered.h264'),'-f','null','-'],check=True,timeout=10)
metadata=json.loads(subprocess.check_output([ffprobe,'-v','error','-count_frames','-select_streams','v:0','-show_entries','stream=codec_name,width,height,nb_read_frames','-of','json',str(directory/'recovered.h264')]))
video=metadata['streams'][0]
assert video['codec_name']=='h264' and video['width']==640 and video['height']==360,video
assert int(video['nb_read_frames'])>=(300 if mode=='pacing' else 30),video
samples=array.array('f');samples.frombytes((directory/'recovered.f32').read_bytes())
assert len(samples)>=20*960*2
assert all(math.isfinite(v) for v in samples)
rms=math.sqrt(sum(v*v for v in samples)/len(samples))
assert .005<rms<.2,rms
producer=('sustained 60 fps console capture bridge: unchanged coded fixture slices, user_data_unregistered SEI NALs, 90 KB actual IDR bursts every two seconds' if mode=='pacing' else
          'console capture bridge (pbsd_capture_*): bare SPS/PPS NALs, 3-byte Annex-B access units, irregular audio blocks, recovery discontinuity' if mode=='pbs' else 'embedded clip fixture')
report={'producer':producer,'video':video,'audio':{'format':'float32 stereo 48000Hz','samples':len(samples),'rms':rms},'scope':'Real TLS/WebSocket, UDP discovery, MLS/DAVE and transport encryption, H264 RTP/reassembly/decoding, Opus encode/decode on loopback; no Discord service or console.'}
if mode=='pacing':
    metrics=json.loads((directory/'pacing-metrics.json').read_text())
    assert metrics['receiver_frames']>=300 and metrics['receiver_span_ms']>=5000,metrics
    assert metrics['stable_drops']==0 and metrics['receiver_max_gap_ms']<200,metrics
    assert metrics['observed_video_mbps']>=3.3,metrics
    assert metrics['received_pacing_sei_frames']>=300 and metrics['max_pacing_idr_bytes']>=449000,metrics
    assert b'PBSDpacingtest01' in (directory/'recovered.h264').read_bytes()
    report['pacing']=metrics
(directory/'verification.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report))
