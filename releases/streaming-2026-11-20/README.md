# PBS Cord streaming-only build

This PS5 build fixes missing Discord game icons. It uses Discord's media proxy for activity artwork and can look up PS4 icons by PlayStation title ID. The source also includes a bulk artwork cache tool for consoles that cannot reach PlayStation's artwork metadata service. Experimental two-way voice chat is disabled.

Download `pbs-discord.elf` and load it with your PS5 payload loader. The matching source and build instructions are available below.

- [Source commit](https://github.com/tanner02/PBS-Cord/tree/4f0e5869e1937c6bd5f8df97680dcaee090618be)
- [Build instructions](https://github.com/tanner02/PBS-Cord/blob/4f0e5869e1937c6bd5f8df97680dcaee090618be/BUILDING.md)
- [Component licenses](https://github.com/tanner02/PBS-Cord/blob/4f0e5869e1937c6bd5f8df97680dcaee090618be/THIRD_PARTY.md)

The PS5 cross-build and all 24 host tests passed. This exact ELF has not been run on a PS5.

SHA-256 (`pbs-discord.elf`)

```text
4267afeba80350e53242a63fd003ed900322276a740d75782f5ad989f4ad23ef
```
