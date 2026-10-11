#!/usr/bin/env python3
"""Windows host for PC Voice.

The AI Passport streams speech over Bluetooth. This program transcribes it
and types the text into the focused Windows application.
"""

from __future__ import annotations

import array
import asyncio
import os
import sys
import tempfile
import threading
import time
import wave

from protocol import (
    DEVICE_NAME,
    END_FAIL,
    EVENT_UUID,
    FAIL,
    FRAME_AUDIO,
    FRAME_AUDIO_END,
    FRAME_AUDIO_START,
    FRAME_CLEAR,
    FRAME_DELETE,
    FRAME_SEND,
    HELLO,
    SERVICE_UUID,
    TEXT_UUID,
    choose_language,
    decode_audio_frame,
    fallback_language,
    lang_ids,
    language_label,
    self_test,
    start_rate,
    transcript_packets,
)

INK = "#07110F"
LINE = "#39F2C6"
TEXT = "#D7FFF4"
DIM = "#7E9E96"


def _pcm_bytes(samples: list[int]) -> bytes:
    data = array.array("h", samples)
    if sys.byteorder != "little":
        data.byteswap()
    return data.tobytes()


class SpeechEngine:
    """Offline transcription. Windows Speech is used when a language is installed."""

    def __init__(self) -> None:
        self.label = "LISTENING"
        self._mode = ""
        self._lang = ""
        self._ready = threading.Event()
        self._error = ""
        threading.Thread(target=self._boot, name="pcvoice-speech", daemon=True).start()

    def _boot(self) -> None:
        try:
            installed = _sapi_languages()
            chosen = choose_language(installed)
            if chosen:
                self._mode = "sapi"
                self._lang = chosen
                self.label = language_label(chosen)
            else:
                _load_whisper()
                self._mode = "whisper"
                self.label = "CANTONESE"
        except Exception as exc:
            self._mode = ""
            self._error = str(exc)
        finally:
            self._ready.set()

    def ready(self) -> bool:
        return self._ready.is_set() and self._mode in ("sapi", "whisper")

    def transcribe(self, samples: list[int], rate: int) -> str:
        try:
            if not self._ready.wait(8):
                return ""
            if self._mode == "sapi":
                text = _sapi_recognize(samples, rate, self._lang, seconds=4)
                if not text:
                    text = _sapi_recognize(samples, rate, fallback_language(self._lang), seconds=4)
                return text.strip()
            if self._mode == "whisper":
                return _whisper_transcribe(samples, rate).strip()
        except Exception:
            return ""
        return ""


_whisper_model = None
_whisper_lock = threading.Lock()


def _load_whisper():
    global _whisper_model
    with _whisper_lock:
        if _whisper_model is None:
            from faster_whisper import WhisperModel

            name = os.environ.get("PC_VOICE_MODEL", "base")
            _whisper_model = WhisperModel(name, device="cpu", compute_type="int8")
        return _whisper_model


def _to_float16k(samples: list[int], rate: int):
    import numpy as np

    audio = np.asarray(samples, dtype=np.float32)
    if rate > 0 and rate != 16000 and len(audio) > 1:
        count = max(1, int(round(len(audio) * 16000 / float(rate))))
        source_x = np.linspace(0.0, 1.0, num=len(audio), endpoint=False)
        target_x = np.linspace(0.0, 1.0, num=count, endpoint=False)
        audio = np.interp(target_x, source_x, audio).astype(np.float32)
    return audio / 32768.0


def _whisper_transcribe(samples: list[int], rate: int) -> str:
    model = _load_whisper()
    audio = _to_float16k(samples, rate)
    try:
        segments, _info = model.transcribe(
            audio, language="yue", beam_size=1, vad_filter=True, initial_prompt="粵語"
        )
    except Exception:
        segments, _info = model.transcribe(
            audio, language="zh", beam_size=1, vad_filter=True, initial_prompt="粵語"
        )
    return "".join(segment.text for segment in segments)


def _sapi_languages() -> list[str]:
    try:
        import pythoncom
        import win32com.client
    except ImportError:
        return []
    pythoncom.CoInitialize()
    try:
        recognizer = win32com.client.Dispatch("SAPI.SpInprocRecognizer")
        try:
            tokens = recognizer.GetRecognizers("", "")
        except Exception:
            tokens = recognizer.GetRecognizers()
        found: list[str] = []
        for index in range(int(tokens.Count)):
            found.append(str(tokens.Item(index).GetAttribute("Language")))
        return found
    except Exception:
        return []
    finally:
        pythoncom.CoUninitialize()


class _RecoHolder:
    text = ""
    done = False


class _RecoEvents:
    def OnRecognition(self, _stream, _position, _kind, result) -> None:
        try:
            _RecoHolder.text = result.PhraseInfo.GetText()
        except Exception:
            _RecoHolder.text = ""
        _RecoHolder.done = True

    def OnFalseRecognition(self, _stream, _position, _result) -> None:
        _RecoHolder.done = True

    def OnEndStream(self, _stream, _position, _released) -> None:
        _RecoHolder.done = True


def _sapi_recognize(samples: list[int], rate: int, lang_id: str, seconds: float) -> str:
    if not samples:
        return ""
    try:
        import pythoncom
        import win32com.client
    except ImportError:
        return ""
    pythoncom.CoInitialize()
    path = ""
    try:
        recognizer = win32com.client.Dispatch("SAPI.SpInprocRecognizer")
        try:
            tokens = recognizer.GetRecognizers("", "")
        except Exception:
            tokens = recognizer.GetRecognizers()
        token = None
        for index in range(int(tokens.Count)):
            item = tokens.Item(index)
            if lang_id in lang_ids(str(item.GetAttribute("Language"))):
                token = item
                break
        if token is None:
            return ""
        recognizer.Recognizer = token
        fd, path = tempfile.mkstemp(suffix=".wav")
        os.close(fd)
        with wave.open(path, "wb") as handle:
            handle.setnchannels(1)
            handle.setsampwidth(2)
            handle.setframerate(rate if rate > 0 else 16000)
            handle.writeframes(_pcm_bytes(samples))
        stream = win32com.client.Dispatch("SAPI.SpFileStream")
        try:
            stream.Open(path, 0, False)
        except TypeError:
            stream.Open(path, 0)
        recognizer.AudioInputStream = stream
        _RecoHolder.text = ""
        _RecoHolder.done = False
        context = win32com.client.DispatchWithEvents(recognizer.CreateRecoContext(), _RecoEvents)
        grammar = context.CreateGrammar()
        grammar.DictationLoad()
        grammar.DictationSetState(1)
        recognizer.State = 1
        deadline = time.time() + seconds
        while time.time() < deadline and not _RecoHolder.done:
            pythoncom.PumpWaitingMessages()
            time.sleep(0.05)
        recognizer.State = 0
        return _RecoHolder.text or ""
    except Exception:
        return ""
    finally:
        pythoncom.CoUninitialize()
        if path:
            try:
                os.remove(path)
            except OSError:
                pass


class Keys:
    _ready = False

    @classmethod
    def _bind(cls):
        if cls._ready:
            return
        import ctypes
        from ctypes import wintypes

        cls.user32 = ctypes.WinDLL("user32", use_last_error=True)
        cls.INPUT_KEYBOARD = 1
        cls.KEYEVENTF_KEYUP = 0x0002
        cls.KEYEVENTF_UNICODE = 0x0004
        cls.VK_BACK = 0x08
        cls.VK_RETURN = 0x0D
        cls.GA_ROOT = 2

        class KEYBDINPUT(ctypes.Structure):
            _fields_ = (
                ("wVk", wintypes.WORD),
                ("wScan", wintypes.WORD),
                ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.c_size_t),
            )

        class MOUSEINPUT(ctypes.Structure):
            _fields_ = (
                ("dx", wintypes.LONG),
                ("dy", wintypes.LONG),
                ("mouseData", wintypes.DWORD),
                ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.c_size_t),
            )

        class HARDWAREINPUT(ctypes.Structure):
            _fields_ = (
                ("uMsg", wintypes.DWORD),
                ("wParamL", wintypes.WORD),
                ("wParamH", wintypes.WORD),
            )

        class INPUT(ctypes.Structure):
            class _U(ctypes.Union):
                _fields_ = (("ki", KEYBDINPUT), ("mi", MOUSEINPUT), ("hi", HARDWAREINPUT))

            _anonymous_ = ("u",)
            _fields_ = (("type", wintypes.DWORD), ("u", _U))

        cls.INPUT = INPUT
        cls.KEYBDINPUT = KEYBDINPUT
        cls.user32.SendInput.argtypes = (wintypes.UINT, ctypes.POINTER(INPUT), ctypes.c_int)
        cls.user32.SendInput.restype = wintypes.UINT
        cls._ready = True

    @classmethod
    def _key(cls, virtual: int, scan: int, flags: int):
        item = cls.INPUT()
        item.type = cls.INPUT_KEYBOARD
        item.ki = cls.KEYBDINPUT(virtual, scan, flags, 0, 0)
        return item

    @classmethod
    def _send(cls, items: list) -> None:
        cls._bind()
        array_type = cls.INPUT * len(items)
        packed = array_type(*items)
        cls.user32.SendInput(len(items), packed, __import__("ctypes").sizeof(cls.INPUT))

    @classmethod
    def type_text(cls, text: str) -> None:
        if not text:
            return
        cls._bind()
        encoded = text.encode("utf-16-le")
        items = []
        for index in range(0, len(encoded), 2):
            code = int.from_bytes(encoded[index:index + 2], "little")
            items.append(cls._key(0, code, cls.KEYEVENTF_UNICODE))
            items.append(cls._key(0, code, cls.KEYEVENTF_UNICODE | cls.KEYEVENTF_KEYUP))
        cls._send(items)

    @classmethod
    def backspace(cls, count: int) -> None:
        if count <= 0:
            return
        cls._bind()
        for _ in range(count):
            cls._send([
                cls._key(cls.VK_BACK, 0, 0),
                cls._key(cls.VK_BACK, 0, cls.KEYEVENTF_KEYUP),
            ])
            time.sleep(0.012)

    @classmethod
    def send_key(cls) -> None:
        cls._bind()
        cls._send([
            cls._key(cls.VK_RETURN, 0, 0),
            cls._key(cls.VK_RETURN, 0, cls.KEYEVENTF_KEYUP),
        ])

    @classmethod
    def foreground(cls) -> int:
        cls._bind()
        return int(cls.user32.GetForegroundWindow() or 0)

    @classmethod
    def focus(cls, hwnd: int) -> None:
        if hwnd:
            cls._bind()
            cls.user32.SetForegroundWindow(hwnd)

    @classmethod
    def root_hwnd(cls, widget_id: int) -> int:
        cls._bind()
        return int(cls.user32.GetAncestor(widget_id, cls.GA_ROOT) or widget_id)


def _link_status(exc: BaseException) -> str:
    name = type(exc).__name__.lower()
    text = str(exc).lower()
    if "notavailable" in name or "powered off" in text or "turned off" in text:
        return "BLUETOOTH OFF"
    if "unauthorized" in text or "access is denied" in text or "permission" in text:
        return "ALLOW BLUETOOTH"
    return "SEARCHING"


class VoiceWindow:
    def __init__(self) -> None:
        import tkinter as tk
        import tkinter.font as tkfont

        self.tk = tk
        self.root = tk.Tk()
        self.root.title("PC Voice")
        self.root.geometry("440x560")
        self.root.minsize(420, 520)
        self.root.configure(bg=INK)
        self.status = tk.StringVar(value="NO LINK")
        self.transcript = tk.StringVar(value=" ")
        self.target = 0
        self.own_hwnd = 0
        self.family = self._font_family(tkfont)
        self._build()
        self.link = VoiceLink(self)
        self.root.after(200, self._begin)
        self.root.protocol("WM_DELETE_WINDOW", self.close)

    def _font_family(self, tkfont) -> str:
        import ctypes

        path = os.path.abspath(os.path.join(
            os.path.dirname(__file__), "..", "..", "assets", "fonts", "Orbitron-Variable.ttf"
        ))
        if os.path.isfile(path):
            ctypes.windll.gdi32.AddFontResourceExW(path, 0x10, 0)
        families = set(tkfont.families())
        for name in ("Orbitron", "Consolas", "Segoe UI"):
            if name in families:
                return name
        return "TkFixedFont"

    def _build(self) -> None:
        tk = self.tk
        canvas = tk.Canvas(self.root, width=180, height=180, bg=INK, highlightthickness=0)
        canvas.create_oval(4, 4, 176, 176, outline=LINE, width=2)
        canvas.place(relx=0.5, rely=0.58, anchor="center")
        frame = tk.Frame(self.root, bg=INK)
        frame.place(x=28, y=28, relwidth=1, width=-56)
        tk.Label(frame, text="PC VOICE", bg=INK, fg=LINE, font=(self.family, 22), anchor="w").pack(fill="x")
        tk.Label(frame, textvariable=self.status, bg=INK, fg=TEXT, font=(self.family, 28), anchor="w").pack(
            fill="x", pady=(18, 0)
        )
        tk.Label(
            frame, textvariable=self.transcript, bg=INK, fg=TEXT, font=(self.family, 16),
            anchor="nw", justify="left", wraplength=360, height=4,
        ).pack(fill="x", pady=(18, 0))
        hint = "Cantonese. Hold OK on AI Passport. Release to finish."
        help_text = (
            "It connects by itself. Do not pair it in Windows Settings. "
            "Hold OK to speak. UP deletes one character. DOWN sends."
        )
        for copy in (hint, help_text):
            tk.Label(
                frame, text=copy, bg=INK, fg=DIM, font=(self.family, 16),
                anchor="w", justify="left", wraplength=360,
            ).pack(fill="x", pady=(18, 0))

    def _begin(self) -> None:
        self.own_hwnd = Keys.root_hwnd(self.root.winfo_id())
        self.link.start()

    def set_status(self, text: str) -> None:
        self.root.after(0, lambda: self.status.set(text))

    def set_transcript(self, text: str) -> None:
        shown = text if text else " "
        self.root.after(0, lambda: self.transcript.set(shown))

    def remember_target(self) -> None:
        hwnd = Keys.foreground()
        if hwnd and hwnd != self.own_hwnd:
            self.target = hwnd

    def hide(self) -> None:
        self.root.after(0, self.root.withdraw)

    def reveal(self, status: str) -> None:
        def show() -> None:
            self.status.set(status)
            self.root.deiconify()
            self.root.lift()

        self.root.after(0, show)

    def close(self) -> None:
        self.link.stop()
        self.root.destroy()

    def run(self) -> None:
        self.root.mainloop()


class VoiceLink:
    def __init__(self, window: VoiceWindow) -> None:
        self.window = window
        self.speech = SpeechEngine()
        self.loop: asyncio.AbstractEventLoop | None = None
        self._stop = threading.Event()
        self._client = None
        self._write_length = 20
        self._samples: list[int] = []
        self._rate = 16000
        self._heard = 0
        self._generation = 0
        self._lock = threading.Lock()

    def start(self) -> None:
        threading.Thread(target=self._thread, name="pcvoice-ble", daemon=True).start()

    def stop(self) -> None:
        self._stop.set()
        self._generation += 1
        client = self._client
        loop = self.loop
        if client is not None and loop is not None and loop.is_running():
            asyncio.run_coroutine_threadsafe(client.disconnect(), loop)

    def _thread(self) -> None:
        asyncio.run(self._run())

    async def _run(self) -> None:
        self.loop = asyncio.get_running_loop()
        while not self._stop.is_set():
            try:
                await self._session()
            except Exception as exc:
                if self._stop.is_set():
                    return
                self.window.reveal(_link_status(exc))
                await asyncio.sleep(1.0)

    async def _session(self) -> None:
        from bleak import BleakClient, BleakScanner

        self.window.set_status("SEARCHING")
        found: asyncio.Future = asyncio.get_running_loop().create_future()

        def on_detect(device, adv) -> None:
            if found.done() or self._stop.is_set():
                return
            name = device.name or getattr(adv, "local_name", None)
            uuids = [item.lower() for item in (getattr(adv, "service_uuids", None) or [])]
            if name == DEVICE_NAME or SERVICE_UUID in uuids:
                found.set_result(device)

        scanner = BleakScanner(detection_callback=on_detect)
        await scanner.start()
        try:
            device = await asyncio.wait_for(found, timeout=8)
        except asyncio.TimeoutError:
            return
        finally:
            await scanner.stop()

        disconnected = asyncio.Event()

        def on_disconnect(_client) -> None:
            if self.loop is not None:
                self.loop.call_soon_threadsafe(disconnected.set)

        self.window.set_status("WAIT")
        client = BleakClient(device, disconnected_callback=on_disconnect)
        self._client = client
        await client.connect()
        try:
            await client.start_notify(EVENT_UUID, self._on_notify)
            payload = 0
            for _ in range(15):
                try:
                    payload = max(0, int(client.mtu_size) - 3)
                except Exception:
                    payload = 0
                if payload >= 168:
                    break
                await asyncio.sleep(0.2)
            self._write_length = max(20, payload)
            await client.write_gatt_char(TEXT_UUID, HELLO, response=False)
            self.window.set_status("HOLD OK" if payload >= 168 else "LINK SLOW")
            await disconnected.wait()
        finally:
            self._client = None
            self._generation += 1
            with self._lock:
                self._samples = []
                self._heard = 0
            if client.is_connected:
                await client.disconnect()
            if not self._stop.is_set():
                self.window.reveal("SEARCHING")

    def _on_notify(self, _sender, data) -> None:
        payload = bytes(data)
        if not payload:
            return
        kind = payload[0]
        if kind == FRAME_AUDIO_START:
            rate = start_rate(payload)
            if rate is None:
                return
            self._generation += 1
            with self._lock:
                self._rate = rate
                self._samples = []
                self._heard = 0
            self.window.remember_target()
            if self.speech.ready():
                self.window.set_status(self.speech.label)
            elif self.speech._ready.is_set():
                self.window.reveal("NO SPEECH")
            else:
                self.window.set_status("LOADING SPEECH")
            return
        if kind == FRAME_AUDIO:
            decoded = decode_audio_frame(payload)
            if not decoded:
                return
            with self._lock:
                self._heard += 1
                self._samples.extend(decoded)
            return
        if kind == FRAME_AUDIO_END:
            flags = payload[1] if len(payload) > 1 else 0
            generation = self._generation
            with self._lock:
                samples = list(self._samples)
                rate = self._rate
                heard = self._heard
                self._samples = []
            if flags & END_FAIL or heard == 0 or (self.speech._ready.is_set() and not self.speech.ready()):
                if self.speech._ready.is_set() and not self.speech.ready():
                    status = "NO SPEECH"
                elif heard == 0 and not (flags & END_FAIL):
                    status = "NO AUDIO"
                else:
                    status = "TRY AGAIN"
                self._fail_async(status)
                return
            self.window.set_status("WORKING")
            threading.Thread(
                target=self._finish, args=(generation, samples, rate), name="pcvoice-recog", daemon=True
            ).start()
            return
        if kind == FRAME_DELETE:
            threading.Thread(target=lambda: Keys.backspace(1), daemon=True).start()
            return
        if kind == FRAME_SEND:
            threading.Thread(target=Keys.send_key, daemon=True).start()
            self.window.set_status("SENT")
            return
        if kind == FRAME_CLEAR and len(payload) >= 3:
            count = int.from_bytes(payload[1:3], "little")
            threading.Thread(target=lambda n=count: Keys.backspace(n), daemon=True).start()

    def _finish(self, generation: int, samples: list[int], rate: int) -> None:
        text = self.speech.transcribe(samples, rate)
        if generation != self._generation:
            return
        trimmed = text.strip()
        if not trimmed:
            self._fail_blocking("TRY AGAIN")
            return
        self.window.set_transcript(trimmed)
        time.sleep(0.12)
        if generation != self._generation:
            return
        try:
            self._write_bytes(transcript_packets(trimmed, self._write_length))
        except Exception:
            self._fail_blocking("TRY AGAIN")
            return
        self.window.hide()
        time.sleep(0.35)
        if generation != self._generation:
            return
        Keys.focus(self.window.target)
        Keys.type_text(trimmed)
        self.window.set_status("UP DEL    DOWN SEND")

    def _fail_async(self, status: str) -> None:
        loop = self.loop
        client = self._client

        def schedule() -> None:
            if client is not None:
                asyncio.create_task(self._write_all(client, [FAIL]))

        if loop is not None and loop.is_running():
            loop.call_soon_threadsafe(schedule)
        self.window.reveal(status)

    def _fail_blocking(self, status: str) -> None:
        try:
            self._write_bytes([FAIL])
        except Exception:
            pass
        self.window.reveal(status)

    def _write_bytes(self, packets: list[bytes]) -> None:
        loop = self.loop
        client = self._client
        if loop is None or client is None:
            raise RuntimeError("no link")
        future = asyncio.run_coroutine_threadsafe(self._write_all(client, packets), loop)
        future.result(timeout=3)

    async def _write_all(self, client, packets: list[bytes]) -> None:
        for packet in packets:
            await client.write_gatt_char(TEXT_UUID, packet, response=False)


def _enable_dpi() -> None:
    try:
        import ctypes
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except Exception:
        try:
            import ctypes
            ctypes.windll.user32.SetProcessDPIAware()
        except Exception:
            pass


def main() -> int:
    if "--self-test" in sys.argv:
        ok = self_test()
        print("pc voice protocol self-test: PASS" if ok else "pc voice protocol self-test: FAIL")
        return 0 if ok else 1
    if sys.platform != "win32":
        print("PC Voice runs on Windows. Protocol check: python3 protocol.py --self-test")
        return 1
    try:
        import bleak  # noqa: F401
    except ImportError:
        print("Install dependencies first: py -3 -m pip install -r requirements.txt")
        return 1
    _enable_dpi()
    VoiceWindow().run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
