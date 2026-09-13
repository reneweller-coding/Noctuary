"""ImpulseGen: impulse responses for Noctuary's Room (convolution reverb), with a GUI.

Tabs: Design (procedural rooms with per-band RT60, size, pre-delay, width, tone, modulation),
Recording (any WAV or an AI render becomes an impulse: onset, trim, floor, tail extension),
Prompt (a TextureGen model renders "a single clap in <room>", which is then cut to an impulse),
Hybrid (the recording's colour with the designed decay). Preview: the impulse itself and a
short dry chord convolved with it. Export: Impulses/<name>.wav (stereo float).
"""
import json
import os
import random
import sys

import numpy as np
from PySide6.QtCore import QProcess, Qt, QUrl, QRectF
from PySide6.QtGui import QColor, QPainter, QPen, QDesktopServices
from PySide6.QtMultimedia import QAudioOutput, QMediaPlayer
from PySide6.QtWidgets import (QApplication, QCheckBox, QComboBox, QDoubleSpinBox, QFileDialog, QFormLayout, QGroupBox,
                               QHBoxLayout, QLabel, QLineEdit, QMainWindow, QPlainTextEdit, QPushButton, QSpinBox,
                               QTabWidget, QVBoxLayout, QWidget)

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "TextureGen"))
import impulsegen_core as ig  # noqa: E402
from impulsegen_cli import ROOM_PRESETS, TEXTURE_WORKER  # noqa: E402

ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
# Where a hand-run experiment lands. NOT Library/: what the three designers write while
# somebody is trying things out is not the shipping library, and three folders in the repo
# root called Textures, Wavetables and Impulses looked exactly like a second one.
DEFAULT_OUT = os.path.join(ROOT, "Scratch", "Impulses")
try:
    from texturegen_worker import MODELS as TEXTURE_MODELS
except Exception:
    TEXTURE_MODELS = {}

ROOM_IDEAS = ["a huge stone cathedral", "a flooded concrete bunker", "a snowy pine forest at night", "an empty aircraft hangar",
              "a glass greenhouse", "a deep limestone cave", "a wooden chapel", "a subway tunnel", "a marble bathhouse"]


class IrView(QWidget):
    def __init__(self):
        super().__init__()
        self.ir = None; self.sr = 48000
        self.setMinimumHeight(260)

    def set_ir(self, ir, sr):
        self.ir, self.sr = ir, sr; self.update()

    def paintEvent(self, e):
        p = QPainter(self)
        p.fillRect(self.rect(), QColor(24, 26, 32))
        if self.ir is None:
            p.setPen(QColor(120, 125, 140)); p.drawText(self.rect(), Qt.AlignCenter, "no impulse yet"); return
        w, h = self.width(), self.height()
        top = QRectF(8, 8, w - 16, h * 0.55 - 12); bot = QRectF(8, h * 0.55 + 4, w - 16, h * 0.45 - 12)
        p.setPen(QPen(QColor(60, 64, 76), 1)); p.drawRect(top); p.drawRect(bot)
        mono = self.ir.mean(axis=0); n = len(mono)
        cols = int(top.width())
        step = max(1, n // cols)
        # waveform (min/max per column) in dB-ish scaling for the long tail
        p.setPen(QPen(QColor(127, 179, 213), 1))
        for c in range(cols):
            seg = mono[c * step:(c + 1) * step]
            if len(seg) == 0: break
            a = float(np.max(np.abs(seg)))
            y = top.height() * 0.48 * min(1.0, a ** 0.5)
            x = top.left() + c
            p.drawLine(QRectF(x, top.center().y() - y, 0, 0).topLeft(), QRectF(x, top.center().y() + y, 0, 0).topLeft())
        # energy decay curve in dB
        edc = np.cumsum(mono[::-1] ** 2)[::-1]; edc = 10 * np.log10(edc / (edc[0] + 1e-20) + 1e-20)
        p.setPen(QPen(QColor(230, 180, 90), 1.5))
        prev = None
        for c in range(cols):
            i = min(n - 1, c * step)
            y = bot.top() + bot.height() * min(1.0, -edc[i] / 80.0)
            pt = QRectF(bot.left() + c, y, 0, 0).topLeft()
            if prev is not None: p.drawLine(prev, pt)
            prev = pt
        d = ig.describe(self.ir, self.sr)
        p.setPen(QColor(160, 165, 180))
        p.drawText(top.adjusted(6, 4, -6, -4), Qt.AlignLeft | Qt.AlignTop, f"{d['seconds']:.1f} s   RT60 ~{d['rt60_estimate']:.1f} s   L/R correlation {d['stereo_correlation']:.2f}")
        p.drawText(bot.adjusted(6, 4, -6, -4), Qt.AlignLeft | Qt.AlignTop, "energy decay, 0 .. -80 dB")


class Main(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Noctuary ImpulseGen")
        self.resize(1080, 720)
        self.ir = None; self.sr = 48000; self.meta = {}
        self.player = QMediaPlayer(self); self.audio_out = QAudioOutput(self); self.player.setAudioOutput(self.audio_out)
        self.proc = None
        root = QWidget(); self.setCentralWidget(root)
        outer = QHBoxLayout(root)
        left = QVBoxLayout(); outer.addLayout(left, 2)
        right = QVBoxLayout(); outer.addLayout(right, 3)
        self.tabs = QTabWidget(); left.addWidget(self.tabs)

        # --- design
        w = QWidget(); form = QFormLayout(w); self.tabs.addTab(w, "Design")
        self.preset = QComboBox(); self.preset.addItem("custom")
        for k in ROOM_PRESETS: self.preset.addItem(k)
        self.preset.currentIndexChanged.connect(self.load_room_preset); form.addRow("Room", self.preset)
        self.seconds = QDoubleSpinBox(); self.seconds.setRange(0.5, 8); self.seconds.setValue(5); self.seconds.setSuffix(" s"); form.addRow("Length", self.seconds)
        self.rt_low = QDoubleSpinBox(); self.rt_low.setRange(0.1, 20); self.rt_low.setValue(5.0); self.rt_low.setSuffix(" s"); form.addRow("RT60 low (<300 Hz)", self.rt_low)
        self.rt_mid = QDoubleSpinBox(); self.rt_mid.setRange(0.1, 20); self.rt_mid.setValue(3.0); self.rt_mid.setSuffix(" s"); form.addRow("RT60 mid", self.rt_mid)
        self.rt_high = QDoubleSpinBox(); self.rt_high.setRange(0.05, 20); self.rt_high.setValue(1.2); self.rt_high.setSuffix(" s"); form.addRow("RT60 high (>3 kHz)", self.rt_high)
        self.size = QDoubleSpinBox(); self.size.setRange(0.1, 4); self.size.setValue(1.0); self.size.setSingleStep(0.1); form.addRow("Size", self.size)
        self.predelay = QDoubleSpinBox(); self.predelay.setRange(0, 300); self.predelay.setValue(0); self.predelay.setSuffix(" ms"); form.addRow("Pre-delay", self.predelay)
        self.width = QDoubleSpinBox(); self.width.setRange(0, 1); self.width.setValue(1.0); self.width.setSingleStep(0.1); form.addRow("Width", self.width)
        self.tone = QDoubleSpinBox(); self.tone.setRange(-1, 1); self.tone.setValue(0.0); self.tone.setSingleStep(0.1); form.addRow("Tone (dark .. bright)", self.tone)
        self.modulation = QDoubleSpinBox(); self.modulation.setRange(0, 1); self.modulation.setValue(0.0); self.modulation.setSingleStep(0.1); form.addRow("Modulation", self.modulation)
        self.seed = QSpinBox(); self.seed.setRange(0, 99999); self.seed.setValue(1); form.addRow("Seed", self.seed)
        b = QPushButton("Design"); b.clicked.connect(self.do_design); form.addRow("", b)

        # --- recording
        w = QWidget(); form = QFormLayout(w); self.tabs.addTab(w, "Recording")
        row = QHBoxLayout(); self.rec_path = QLineEdit(); pb = QPushButton("..."); pb.setFixedWidth(32); pb.clicked.connect(self.pick_rec)
        row.addWidget(self.rec_path); row.addWidget(pb); form.addRow("File", row)
        self.max_seconds = QDoubleSpinBox(); self.max_seconds.setRange(0.5, 12); self.max_seconds.setValue(8); self.max_seconds.setSuffix(" s"); form.addRow("Max length", self.max_seconds)
        self.floor = QDoubleSpinBox(); self.floor.setRange(-90, -20); self.floor.setValue(-60); self.floor.setSuffix(" dB"); form.addRow("Tail floor", self.floor)
        self.extend = QCheckBox("extend a cut-off tail synthetically"); form.addRow("", self.extend)
        self.rt_extend = QDoubleSpinBox(); self.rt_extend.setRange(0.2, 20); self.rt_extend.setValue(3.0); self.rt_extend.setSuffix(" s"); form.addRow("Extension RT60", self.rt_extend)
        b = QPushButton("Cut impulse"); b.clicked.connect(self.do_recording); form.addRow("", b)
        b2 = QPushButton("Hybrid: colour of this file, designed decay"); b2.clicked.connect(self.do_hybrid); form.addRow("", b2)

        # --- prompt
        w = QWidget(); form = QFormLayout(w); self.tabs.addTab(w, "Prompt")
        self.model = QComboBox()
        for key, (label, hub, sr, mx, note) in TEXTURE_MODELS.items(): self.model.addItem(label, key)
        form.addRow("Model", self.model)
        self.room_text = QLineEdit(random.choice(ROOM_IDEAS)); form.addRow("Room", self.room_text)
        idea = QPushButton("Idea"); idea.clicked.connect(lambda: self.room_text.setText(random.choice(ROOM_IDEAS))); form.addRow("", idea)
        self.p_seconds = QDoubleSpinBox(); self.p_seconds.setRange(3, 47); self.p_seconds.setValue(15); self.p_seconds.setSuffix(" s"); form.addRow("Render length", self.p_seconds)
        self.p_seed = QSpinBox(); self.p_seed.setRange(0, 2_000_000_000); self.p_seed.setValue(1); form.addRow("Seed", self.p_seed)
        self.p_extend = QCheckBox("extend the tail if the render ends early"); self.p_extend.setChecked(True); form.addRow("", self.p_extend)
        gen = QPushButton("Generate clap and cut impulse"); gen.clicked.connect(self.do_prompt); form.addRow("", gen)
        if not TEXTURE_MODELS:
            gen.setEnabled(False); form.addRow("", QLabel("TextureGen is not set up (see Tools/TextureGen/README.md)"))

        common = QGroupBox("Impulse"); left.addWidget(common); form = QFormLayout(common)
        self.name = QLineEdit("my_room"); form.addRow("Name", self.name)
        row = QHBoxLayout(); self.out_dir = QLineEdit(DEFAULT_OUT); pb = QPushButton("..."); pb.setFixedWidth(32); pb.clicked.connect(self.pick_dir)
        ob = QPushButton("Open"); ob.setFixedWidth(50); ob.clicked.connect(lambda: QDesktopServices.openUrl(QUrl.fromLocalFile(self.out_dir.text())))
        row.addWidget(self.out_dir); row.addWidget(pb); row.addWidget(ob); form.addRow("Folder", row)
        ex = QPushButton("Export WAV"); ex.clicked.connect(self.do_export); form.addRow("", ex)
        self.status = QLabel("idle"); self.status.setWordWrap(True); left.addWidget(self.status)
        self.log = QPlainTextEdit(); self.log.setReadOnly(True); self.log.setMaximumBlockCount(300); left.addWidget(self.log, 1)

        self.view = IrView(); right.addWidget(self.view, 1)
        prow = QHBoxLayout(); right.addLayout(prow)
        play_ir = QPushButton("Play impulse"); play_ir.clicked.connect(lambda: self.play(False)); prow.addWidget(play_ir)
        play_ch = QPushButton("Play chord through it"); play_ch.clicked.connect(lambda: self.play(True)); prow.addWidget(play_ch)
        stop = QPushButton("Stop"); stop.clicked.connect(self.player.stop); prow.addWidget(stop)
        prow.addStretch(1)
        hint = QLabel("In Noctuary: BACKGROUND → Room → Impulse... → the exported file, then Room Level up. Source Far reverberates the far sends, Near the finished foreground.")
        hint.setWordWrap(True); right.addWidget(hint)

    # ---------------------------------------------------------------- actions
    def set_ir(self, ir, sr, meta, source):
        self.ir, self.sr, self.meta = ir, sr, dict(meta); self.meta["source"] = source
        self.view.set_ir(ir, sr)
        d = ig.describe(ir, sr)
        self.status.setText(f"{source}: {d['seconds']:.1f} s, RT60 ~{d['rt60_estimate']:.1f} s")
        self.append(self.status.text())

    def load_room_preset(self):
        k = self.preset.currentText()
        if k not in ROOM_PRESETS: return
        sec, lo, mid, hi, size, pre, width, tone, mod = ROOM_PRESETS[k]
        self.seconds.setValue(sec); self.rt_low.setValue(lo); self.rt_mid.setValue(mid); self.rt_high.setValue(hi)
        self.size.setValue(size); self.predelay.setValue(pre); self.width.setValue(width); self.tone.setValue(tone); self.modulation.setValue(mod)
        self.name.setText(k)

    def do_design(self):
        ir = ig.procedural(48000, self.seconds.value(), self.rt_low.value(), self.rt_mid.value(), self.rt_high.value(), self.size.value(),
                           self.predelay.value(), 25.0, self.width.value(), 0.5, self.modulation.value(), self.tone.value(), self.seed.value())
        self.set_ir(ir, 48000, {"mode": "procedural", "rt60": [self.rt_low.value(), self.rt_mid.value(), self.rt_high.value()], "size": self.size.value(),
                                "predelay_ms": self.predelay.value(), "width": self.width.value(), "tone": self.tone.value(), "modulation": self.modulation.value(), "seed": self.seed.value()}, "designed room")

    def do_recording(self):
        path = self.rec_path.text().strip()
        if not os.path.isfile(path): self.append("pick a file first"); return
        try:
            x, sr = ig.read_wav(path)
            ir = ig.from_audio(x, sr, self.max_seconds.value(), self.floor.value(), self.extend.isChecked(), self.rt_extend.value())
            self.set_ir(ir, sr, {"mode": "from audio", "source_file": path, "extend": self.extend.isChecked()}, os.path.basename(path))
            self.name.setText(ig.slugify(os.path.splitext(os.path.basename(path))[0]) + "_ir")
        except Exception as e:
            self.append(f"ERROR {e}")

    def do_hybrid(self):
        path = self.rec_path.text().strip()
        if not os.path.isfile(path): self.append("pick a file first"); return
        try:
            x, sr = ig.read_wav(path)
            ir = ig.hybrid(x, sr, self.seconds.value(), self.rt_low.value(), self.rt_mid.value(), self.rt_high.value(), self.seed.value())
            self.set_ir(ir, sr, {"mode": "hybrid", "source_file": path}, "hybrid of " + os.path.basename(path))
            self.name.setText(ig.slugify(os.path.splitext(os.path.basename(path))[0]) + "_hybrid")
        except Exception as e:
            self.append(f"ERROR {e}")

    def do_prompt(self):
        if self.proc is not None and self.proc.state() != QProcess.NotRunning:
            self.append("still generating"); return
        tmp = os.path.join(self.out_dir.text(), "_prompts"); os.makedirs(tmp, exist_ok=True)
        prompt = f"a single sharp hand clap in {self.room_text.text().strip()}, then only the room's reverb tail, no music, no voices"
        args = [TEXTURE_WORKER, "--model", self.model.currentData(), "--prompt", prompt, "--seconds", str(self.p_seconds.value()),
                "--seed", str(self.p_seed.value()), "--out-dir", tmp, "--steps", "100", "--negative", "music, melody, drums, voice, speech, rhythm"]
        self.proc = QProcess(self); self.proc.setProgram(sys.executable); self.proc.setArguments(args)
        self.proc.readyReadStandardOutput.connect(self.on_worker_out)
        self.proc.readyReadStandardError.connect(lambda: None)
        self.proc.start()
        self.status.setText("rendering the clap ... (first run downloads the model)")

    def on_worker_out(self):
        text = bytes(self.proc.readAllStandardOutput()).decode("utf-8", "replace")
        for line in text.splitlines():
            if not line.startswith("{"): continue
            try: ev = json.loads(line)
            except json.JSONDecodeError: continue
            if ev.get("event") == "status": self.status.setText(ev["text"])
            elif ev.get("event") == "error": self.append("ERROR " + ev["text"])
            elif ev.get("event") == "done":
                self.rec_path.setText(ev["path"]); self.extend.setChecked(self.p_extend.isChecked()); self.max_seconds.setValue(min(12.0, self.p_seconds.value()))
                self.do_recording()
                self.name.setText(ig.slugify(self.room_text.text()))

    def play(self, chord):
        if self.ir is None: return
        import soundfile as sf
        sr = self.sr
        if chord:
            t = np.arange(int(sr * 1.5)) / sr
            dry = np.zeros_like(t)
            for f in (110.0, 165.0, 220.0, 275.0):
                dry += 0.2 * np.sin(2 * np.pi * f * t) * np.exp(-t * 3)
            y = np.stack([np.convolve(dry, self.ir[c])[: len(dry) + self.ir.shape[1]] for c in range(2)])
        else:
            y = self.ir.copy()
        peak = float(np.max(np.abs(y))) + 1e-9
        y = (0.5 * y / peak).astype(np.float32)
        os.makedirs(self.out_dir.text(), exist_ok=True)
        path = os.path.join(self.out_dir.text(), "_preview.wav")
        self.player.stop(); self.player.setSource(QUrl())
        sf.write(path, y.T, sr)
        self.player.setSource(QUrl.fromLocalFile(path)); self.player.play()

    def do_export(self):
        if self.ir is None: self.append("nothing to export"); return
        os.makedirs(self.out_dir.text(), exist_ok=True)
        path = os.path.join(self.out_dir.text(), ig.slugify(self.name.text() or "room", 60) + ".wav")
        ig.save(path, self.ir, self.sr, self.meta)
        self.append(f"exported {path}"); self.status.setText(f"exported {os.path.basename(path)}")

    def pick_rec(self):
        f, _ = QFileDialog.getOpenFileName(self, "Recording or render", os.path.join(ROOT, "Textures"), "Audio (*.wav *.flac *.aif *.aiff *.ogg)")
        if f: self.rec_path.setText(f)

    def pick_dir(self):
        d = QFileDialog.getExistingDirectory(self, "Impulse folder", self.out_dir.text())
        if d: self.out_dir.setText(d)

    def append(self, text): self.log.appendPlainText(text)


def main():
    app = QApplication(sys.argv)
    w = Main(); w.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
