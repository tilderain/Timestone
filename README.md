MVC3 Rollback :steamhappy:

# Compilation

Makefile on Linux is supported for now (cross compiling a dll)

# Automated dual training

Launch the game with `-autotest=<character>` to start a dual-training pair,
place the selected character in all P1 and P2 slots, and run the unique-command
autotest suite after both instances reach the synchronized battle boundary.
Character names are case-insensitive and may omit spaces and punctuation.
Both game processes close automatically after the suite completes.

Examples:

```text
umvc3.exe -autotest=Ryu
umvc3.exe "-autotest=Phoenix Wright"
umvc3.exe -autotest=27
```

The network address and port come from `rollbacksetting.ini`. NDJSON capture
continues to follow the existing NDJSON logging setting.

# Credits

Armonte- Many reverse engineering and documentation, Shot snapshot, inspiration from his repo (multi asi loader, exception handler)
Hkhann- Reverse engineering, the one who started it all with the OG UMVC3Rollback repo
Ryn (wistfulhopes)- Reverse engineering
anotak- Reverse engineering
Eternal Yoshi- Reverse engineering
Archeroflegend- Framestepper
ermaccer- UMVC3Hook
Vevota- Playtesting
John2143- "Sponsorship"
Those random guys who played with me in a steam lobby one day
