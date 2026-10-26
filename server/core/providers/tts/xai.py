"""xAI Grok Voice TTS (POST /v1/tts, MP3) with EdgeTTS fallback. Durk."""
import os
import time
import uuid
import requests
from datetime import datetime
from config.logger import setup_logging
from core.providers.tts.base import TTSProviderBase

TAG = __name__
logger = setup_logging()


class TTSProvider(TTSProviderBase):
    def __init__(self, config, delete_audio_file):
        super().__init__(config, delete_audio_file)
        self.api_key = config.get("api_key")
        self.url = config.get("url", "https://api.x.ai/v1/tts")
        self.voice = config.get("voice", "rex")
        self.language = config.get("language", "en")
        self.speed = float(config.get("speed", 1.0) or 1.0)
        self.bit_rate = int(config.get("bit_rate", 64000))
        self.audio_file_type = "wav"
        self.normalize = bool(config.get("normalize", True))
        self.fallback_voice = config.get("fallback_voice", "en-US-GuyNeural")
        self.timeout = float(config.get("timeout", 8))
        self._session = requests.Session()

    def generate_filename(self, extension=".wav"):
        return os.path.join(self.output_file, f"tts-{datetime.now().date()}@{uuid.uuid4().hex}{extension}")

    def _xai(self, text):
        t = time.time()
        r = self._session.post(
            self.url,
            headers={"Authorization": f"Bearer {self.api_key}", "Content-Type": "application/json"},
            json={"text": text, "voice_id": self.voice, "language": self.language, "speed": self.speed,
                  "output_format": {"codec": "wav", "sample_rate": 24000}},
            timeout=self.timeout,
        )
        if r.status_code in (429, 500, 503):
            time.sleep(0.4)
            r = self._session.post(r.request.url, headers=r.request.headers, data=r.request.body, timeout=self.timeout)
        r.raise_for_status()
        logger.bind(tag=TAG).info(f"xAI TTS {self.voice} {len(text)} chars -> {len(r.content)} B in {time.time() - t:.2f}s")
        audio = r.content
        if self.normalize:
            try:
                import io
                from pydub import AudioSegment, effects
                seg = effects.normalize(AudioSegment.from_file(io.BytesIO(audio), format="wav"), headroom=1.5)
                buf = io.BytesIO(); seg.export(buf, format="wav"); audio = buf.getvalue()
            except Exception as e:
                logger.bind(tag=TAG).warning(f"normalize skipped: {e}")
        return audio

    async def _edge(self, text):
        import edge_tts
        audio = b""
        async for ch in edge_tts.Communicate(text, voice=self.fallback_voice).stream():
            if ch["type"] == "audio":
                audio += ch["data"]
        return audio

    async def text_to_speak(self, text, output_file):
        import asyncio
        try:
            audio = await asyncio.get_running_loop().run_in_executor(None, self._xai, text)
        except Exception as e:
            logger.bind(tag=TAG).error(f"xAI TTS failed ({e}); falling back to EdgeTTS")
            audio = await self._edge(text)
            try:
                import io
                from pydub import AudioSegment
                buf = io.BytesIO(); AudioSegment.from_file(io.BytesIO(audio), format="mp3").export(buf, format="wav"); audio = buf.getvalue()
            except Exception as e2:
                logger.bind(tag=TAG).error(f"Edge fallback convert failed: {e2}")
        if output_file:
            os.makedirs(os.path.dirname(output_file), exist_ok=True)
            with open(output_file, "wb") as f:
                f.write(audio)
        else:
            return audio
