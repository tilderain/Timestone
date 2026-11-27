"""Verify nothing looks for Discord before the player has said yes."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
CODE = ROOT / "UMVC3Hook/code"
PRESENCE = CODE / "discord_presence.cpp"
GUARD = "if (!DiscordPresence_IsEnabled()) return;"
GUARDED = ("void RegisterLaunchProtocol()", "void Connect(ULONGLONG now)")


def body(text, signature):
    """The braces of the definition of `signature`, declarations skipped."""
    start = 0
    while True:
        at = text.find(signature, start)
        if at < 0:
            raise AssertionError(signature)
        open_at = at + len(signature)
        while open_at < len(text) and text[open_at].isspace():
            open_at += 1
        if open_at < len(text) and text[open_at] == "{":
            break
        start = at + len(signature)
    depth = 0
    for i in range(open_at, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[open_at:i + 1]
    raise AssertionError(signature)


class DiscordConsentTest(unittest.TestCase):
    def test_only_the_presence_module_reaches_discord(self):
        for path in CODE.glob("*.*"):
            if path.suffix not in (".cpp", ".h") or path.name == "discord_presence.cpp":
                continue
            text = path.read_text(errors="replace")
            self.assertNotIn("discord-ipc", text, str(path))
            self.assertNotIn(r"Classes\\discord-", text, str(path))

    def test_presence_module_never_enumerates_processes_or_windows(self):
        text = PRESENCE.read_text()
        for name in ("CreateToolhelp32Snapshot", "EnumProcesses", "Process32First",
                     "OpenProcess", "FindWindow", "EnumWindows", "WaitNamedPipe",
                     "NtQuerySystemInformation"):
            self.assertNotIn(name, text)

    def test_pipe_and_registry_calls_sit_behind_the_switch(self):
        text = PRESENCE.read_text()
        for signature in GUARDED:
            self.assertTrue(body(text, signature)[1:].lstrip().startswith(GUARD), signature)
        bodies = [body(text, signature)
                  for signature in GUARDED + ("void RemoveLaunchProtocol()",)]
        for call in ("CreateFileW(", "RegGetValueW(", "RegCreateKeyExW(",
                     "RegSetKeyValueW(", "RegDeleteTreeW("):
            self.assertEqual(text.count(call), sum(b.count(call) for b in bodies), call)

    def test_worker_starts_only_from_the_enabled_tick(self):
        text = PRESENCE.read_text()
        # Calls only: the forward declaration `void EnsureWorker();` is not one.
        calls = re.compile(r"(?<!void )\bEnsureWorker\(\);")
        self.assertEqual(len(calls.findall(text)), 1)
        tick = body(text, "void DiscordPresence_GameTick()")
        call = calls.search(tick)
        self.assertIsNotNone(call)
        enabled = tick.find("DiscordPresence_IsEnabled()")
        self.assertGreaterEqual(enabled, 0)
        self.assertLess(enabled, call.start())
        self.assertEqual(text.count("CreateThread("), 1)
        self.assertEqual(body(text, "void EnsureWorker()").count("CreateThread("), 1)

    def test_lobby_ui_stays_free_of_discord(self):
        for name in ("lobby_ui.cpp", "lobby_ui.h"):
            self.assertNotIn("discord_presence", (CODE / name).read_text(), name)


if __name__ == "__main__":
    unittest.main()
