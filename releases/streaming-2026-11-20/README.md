# PBS Cord streaming-only build

This PS5 build fixes missing Discord game icons. It uses Discord's media proxy for activity artwork and can look up PS4 icons by PlayStation title ID. The source also includes a bulk artwork cache tool for consoles that cannot reach PlayStation's artwork metadata service. Experimental two-way voice chat is disabled.

Download `pbs-discord.elf` and load it with your PS5 payload loader. The matching source and build instructions are available below.

- [Source commit](https://github.com/tanner02/PBS-Cord/tree/5d8c17a04c098850c7f2b58a86ed34ab53f1fedc)
- [Build instructions](https://github.com/tanner02/PBS-Cord/blob/5d8c17a04c098850c7f2b58a86ed34ab53f1fedc/BUILDING.md)
- [Component licenses](https://github.com/tanner02/PBS-Cord/blob/5d8c17a04c098850c7f2b58a86ed34ab53f1fedc/THIRD_PARTY.md)

The dashboard now listens on TCP port 9878 by default. The PS5 cross-build and the payload argument test passed. This exact ELF has not been run on a PS5.

SHA-256 (`pbs-discord.elf`)

```text
b8514e4c6660be33af6b7146005331179d845bd0abf6a64dd0af5d452c6eaaa8
```
