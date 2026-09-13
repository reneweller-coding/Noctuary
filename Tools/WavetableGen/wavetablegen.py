"""WavetableGen: wavetables for Noctuary's User table (and any 2048-frame wavetable synth).

Three sources:
  Audio     - slice single cycles out of any WAV (a TextureGen result, a recording, a synth note):
              pitch-tracked, averaged over a few cycles, phase-aligned between frames.
  Prompt    - ask TextureGen's models (Stable Audio Open / MusicGen) for a sustained note, then
              slice it. Runs texturegen_worker.py as a child process.
  Procedural - spectral recipes over the table position (saw->square, tilt walk, formant sweep,
              comb, glass, odd breathing, random walk) with optional random walk and phase scatter.
Preview: frames as waveforms and a spectral image; a sweep through the table plays at a chosen note.
Export: Wavetables/<name>.wav in the Serum/Vital layout (frames of 2048 samples back to back).
"""
import json
import os
import subprocess
import sys

import numpy as np
from PySide6.QtCore import QProcess, Qt, QUrl, QRectF
from PySide6.QtGui import QColor, QImage, QPainter, QPen, QDesktopServices
from PySide6.QtMultimedia import QAudioOutput, QMediaPlayer
from PySide6.QtWidgets import (QApplication, QCheckBox, QComboBox, QDoubleSpinBox, QFileDialog, QFormLayout, QGroupBox,
                               QHBoxLayout, QLabel, QLineEdit, QMainWindow, QPlainTextEdit, QPushButton, QSlider,
                               QSpinBox, QTabWidget, QVBoxLayout, QWidget)

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "TextureGen"))
import wavetablegen_core as wt  # noqa: E402

ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
# Where a hand-run experiment lands. NOT Library/: what the three designers write while
# somebody is trying things out is not the shipping library, and three folders in the repo
# root called Textures, Wavetables and Impulses looked exactly like a second one.
DEFAULT_OUT = os.path.join(ROOT, "Scratch", "Wavetables")
TEXTURE_WORKER = os.path.normpath(os.path.join(HERE, "..", "TextureGen", "texturegen_worker.py"))
try:
    from texturegen_worker import MODELS as TEXTURE_MODELS
except Exception:   # TextureGen not set up: the prompt tab is disabled
    TEXTURE_MODELS = {}


class TableView(QWidget):
    """Top: the selected frame as a waveform; bottom: all frames as a spectral image
    (partial 1..48 up, table position across)."""
    def __init__(self):
        super().__init__()
        self.table = None
        self.frame = 0
        self.setMinimumHeight(320)

    def set_table(self, table):
        self.table = table
        self.frame = min(self.frame, (0 if table is None else table.shape[0] - 1))
        self.update()

    def paintEvent(self, e):
        p = QPainter(self)
        p.fillRect(self.rect(), QColor(24, 26, 32))
        if self.table is None:
            p.setPen(QColor(120, 125, 140)); p.drawText(self.rect(), Qt.AlignCenter, "no table yet"); return
        w, h = self.width(), self.height()
        top = QRectF(8, 8, w - 16, h * 0.45 - 12)
        bot = QRectF(8, h * 0.45 + 4, w - 16, h * 0.55 - 12)
        # waveform of the selected frame
        p.setPen(QPen(QColor(60, 64, 76), 1)); p.drawRect(top)
        f = self.table[self.frame]
        n = len(f)
        pts = [(top.left() + top.width() * i / (n - 1), top.center().y() - f[i] * top.height() * 0.48) for i in range(0, n, 4)]
        p.setPen(QPen(QColor(127, 179, 213), 1.5))
        for a, b in zip(pts[:-1], pts[1:]):
            p.drawLine(QRectF(a[0], a[1], 0, 0).topLeft(), QRectF(b[0], b[1], 0, 0).topLeft())
        p.setPen(QColor(160, 165, 180)); p.drawText(top.adjusted(6, 4, -6, -4), Qt.AlignLeft | Qt.AlignTop, f"frame {self.frame + 1} / {self.table.shape[0]}")
        # spectral image
        frames = self.table.shape[0]
        parts = 48
        img = QImage(frames, parts, QImage.Format_RGB32)
        for i in range(frames):
            s = wt.spectrum(self.table[i], parts)
            for k in range(parts):
                v = float(np.clip(np.log10(1e-3 + s[k]) / 3 + 1, 0, 1))   # -60 dB .. 0 dB
                c = QColor(int(20 + 100 * v), int(30 + 150 * v), int(60 + 180 * v))
                img.setPixelColor(i, parts - 1 - k, c)
        p.drawImage(bot, img)
        p.setPen(QPen(QColor(230, 180, 90), 1))
        x = bot.left() + bot.width() * (self.frame + 0.5) / frames
        p.drawLine(QRectF(x, bot.top(), 0, 0).topLeft(), QRectF(x, bot.bottom(), 0, 0).topLeft())
        p.setPen(QColor(160, 165, 180)); p.drawText(bot.adjusted(6, 4, -6, -4), Qt.AlignLeft | Qt.AlignTop, "partials 1..48 over table position")

    def mousePressEvent(self, e):
        self.mouseMoveEvent(e)

    def mouseMoveEvent(self, e):
        if self.table is None:
            return
        frames = self.table.shape[0]
        self.frame = int(np.clip(e.position().x() / max(1, self.width()) * frames, 0, frames - 1))
        self.update()


class Main(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Noctuary WavetableGen")
        self.resize(1100, 760)
        self.table = None
        self.info = {}
        self.player = QMediaPlayer(self); self.audio_out = QAudioOutput(self); self.player.setAudioOutput(self.audio_out)
        self.proc = None
        root = QWidget(); self.setCentralWidget(root)
        outer = QHBoxLayout(root)
        left = QVBoxLayout(); outer.addLayout(left, 2)
        right = QVBoxLayout(); outer.addLayout(right, 3)

        self.tabs = QTabWidget(); left.addWidget(self.tabs)
        # --- audio tab
        w = QWidget(); form = QFormLayout(w); self.tabs.addTab(w, "Audio")
        row = QHBoxLayout(); self.audio_path = QLineEdit(); b = QPushButton("..."); b.setFixedWidth(32); b.clicked.connect(self.pick_audio)
        row.addWidget(self.audio_path); row.addWidget(b); form.addRow("File", row)
        self.start = QDoubleSpinBox(); self.start.setRange(0, 3600); self.start.setSuffix(" s"); form.addRow("Start", self.start)
        self.end = QDoubleSpinBox(); self.end.setRange(0, 3600); self.end.setSuffix(" s"); self.end.setSpecialValueText("end"); form.addRow("End", self.end)
        self.pitch = QDoubleSpinBox(); self.pitch.setRange(0, 4000); self.pitch.setDecimals(2); self.pitch.setSuffix(" Hz"); self.pitch.setSpecialValueText("auto"); form.addRow("Pitch", self.pitch)
        self.cycles = QSpinBox(); self.cycles.setRange(1, 32); self.cycles.setValue(4); form.addRow("Cycles averaged", self.cycles)
        go = QPushButton("Slice"); go.clicked.connect(self.from_audio); form.addRow("", go)
        # --- prompt tab
        w = QWidget(); form = QFormLayout(w); self.tabs.addTab(w, "Prompt")
        self.model = QComboBox()
        for key, (label, hub, sr, mx, note) in TEXTURE_MODELS.items():
            self.model.addItem(label, key)
        form.addRow("Model", self.model)
        self.prompt = QPlainTextEdit("a single sustained note of a bowed glass harmonica, steady pitch, no vibrato, no reverb")
        self.prompt.setFixedHeight(70); form.addRow("Prompt", self.prompt)
        self.p_seconds = QDoubleSpinBox(); self.p_seconds.setRange(2, 47); self.p_seconds.setValue(10); self.p_seconds.setSuffix(" s"); form.addRow("Length", self.p_seconds)
        self.p_seed = QSpinBox(); self.p_seed.setRange(0, 2_000_000_000); self.p_seed.setValue(1); form.addRow("Seed", self.p_seed)
        gen = QPushButton("Generate and slice"); gen.clicked.connect(self.from_prompt); form.addRow("", gen)
        if not TEXTURE_MODELS:
            gen.setEnabled(False); form.addRow("", QLabel("TextureGen is not set up (see Tools/TextureGen/README.md)"))
        # --- procedural tab
        w = QWidget(); form = QFormLayout(w); self.tabs.addTab(w, "Procedural")
        self.recipe = QComboBox(); self.recipe.addItems(list(wt.RECIPES.keys())); form.addRow("Recipe", self.recipe)
        self.partials = QSpinBox(); self.partials.setRange(4, 256); self.partials.setValue(48); form.addRow("Partials", self.partials)
        self.noise = QDoubleSpinBox(); self.noise.setRange(0, 2); self.noise.setSingleStep(0.1); self.noise.setValue(0.2); form.addRow("Random walk", self.noise)
        self.scatter = QDoubleSpinBox(); self.scatter.setRange(0, 1); self.scatter.setSingleStep(0.1); self.scatter.setValue(0.3); form.addRow("Phase scatter", self.scatter)
        self.seed = QSpinBox(); self.seed.setRange(0, 999999); self.seed.setValue(1); form.addRow("Seed", self.seed)
        pb = QPushButton("Build"); pb.clicked.connect(self.from_procedural); form.addRow("", pb)
        mb = QPushButton("Morph current with this recipe"); mb.clicked.connect(self.morph_with_recipe); form.addRow("", mb)

        common = QGroupBox("Table"); left.addWidget(common); form = QFormLayout(common)
        self.frames = QSpinBox(); self.frames.setRange(1, 256); self.frames.setValue(32); form.addRow("Frames", self.frames)
        self.name = QLineEdit("my_table"); form.addRow("Name", self.name)
        row = QHBoxLayout(); self.out_dir = QLineEdit(DEFAULT_OUT); b = QPushButton("..."); b.setFixedWidth(32); b.clicked.connect(self.pick_dir)
        o = QPushButton("Open"); o.setFixedWidth(50); o.clicked.connect(lambda: QDesktopServices.openUrl(QUrl.fromLocalFile(self.out_dir.text())))
        row.addWidget(self.out_dir); row.addWidget(b); row.addWidget(o); form.addRow("Folder", row)
        self.float32 = QCheckBox("32-bit float (else 16-bit)"); self.float32.setChecked(True); form.addRow("", self.float32)
        row = QHBoxLayout()
        self.export = QPushButton("Export WAV"); self.export.clicked.connect(self.do_export)
        row.addWidget(self.export); form.addRow("", row)
        self.status = QLabel("idle"); self.status.setWordWrap(True); left.addWidget(self.status)
        self.log = QPlainTextEdit(); self.log.setReadOnly(True); self.log.setMaximumBlockCount(300); left.addWidget(self.log, 1)

        self.view = TableView(); right.addWidget(self.view, 1)
        prow = QHBoxLayout(); right.addLayout(prow)
        prow.addWidget(QLabel("Preview note"))
        self.note = QComboBox()
        for n in range(24, 84):
            self.note.addItem(f"{wt.NOTE_NAMES[n % 12]}{n // 12 - 1}", n)
        self.note.setCurrentIndex(45 - 24)
        prow.addWidget(self.note)
        self.sweep = QCheckBox("sweep through the table"); self.sweep.setChecked(True); prow.addWidget(self.sweep)
        play = QPushButton("Play"); play.clicked.connect(self.play); prow.addWidget(play)
        stop = QPushButton("Stop"); stop.clicked.connect(self.player.stop); prow.addWidget(stop)
        prow.addStretch(1)
        hint = QLabel("In Noctuary: Source 2/3 → Type Wavetable → Table User → Wavetable... → pick the exported file. "
                      "Position morphs through the frames, Pos Drift lets it wander.")
        hint.setWordWrap(True); right.addWidget(hint)

    # ---------------------------------------------------------------- sources
    def set_table(self, table, info, source):
        self.table, self.info = table, dict(info or {})
        self.info["source"] = source
        self.view.set_table(table)
        pitch = f", pitch {info['pitch_hz']:.1f} Hz ({info['note']})" if info and "pitch_hz" in info else ""
        self.status.setText(f"{table.shape[0]} frames from {source}{pitch}")
        self.append(self.status.text())

    def from_audio(self):
        path = self.audio_path.text().strip()
        if not os.path.isfile(path):
            self.append("pick an audio file first"); return
        try:
            mono, sr = wt.read_wav_mono(path)
            end = self.end.value() if self.end.value() > 0 else None
            pitch = self.pitch.value() if self.pitch.value() > 0 else None
            table, info = wt.table_from_audio(mono, sr, frames=self.frames.value(), pitch_hz=pitch, start=self.start.value(), end=end,
                                              cycles_per_frame=self.cycles.value())
            info["file"] = path
            self.set_table(table, info, os.path.basename(path))
            if not self.name.text() or self.name.text() == "my_table":
                self.name.setText(wt.slugify(os.path.splitext(os.path.basename(path))[0]) + "_" + info["note"])
        except Exception as e:
            self.append(f"ERROR {e}")

    def from_procedural(self):
        table = wt.table_procedural(self.recipe.currentText(), frames=self.frames.value(), partials=self.partials.value(),
                                    seed=self.seed.value(), noise=self.noise.value(), phase_scatter=self.scatter.value())
        self.set_table(table, {"recipe": self.recipe.currentText(), "seed": self.seed.value()}, self.recipe.currentText())
        self.name.setText(wt.slugify(self.recipe.currentText()) + f"_{self.seed.value()}")

    def morph_with_recipe(self):
        if self.table is None:
            self.from_procedural(); return
        other = wt.table_procedural(self.recipe.currentText(), frames=8, partials=self.partials.value(), seed=self.seed.value(),
                                    noise=self.noise.value(), phase_scatter=self.scatter.value())
        table = wt.morph_tables(self.table, other, frames=self.frames.value())
        self.set_table(table, {"morph": self.recipe.currentText()}, f"morph -> {self.recipe.currentText()}")

    def from_prompt(self):
        if self.proc is not None and self.proc.state() != QProcess.NotRunning:
            self.append("still generating"); return
        out_dir = os.path.join(self.out_dir.text(), "_prompts")
        os.makedirs(out_dir, exist_ok=True)
        args = [TEXTURE_WORKER, "--model", self.model.currentData(), "--prompt", self.prompt.toPlainText().strip(),
                "--seconds", str(self.p_seconds.value()), "--seed", str(self.p_seed.value()), "--out-dir", out_dir, "--steps", "100"]
        self.proc = QProcess(self)
        self.proc.setProgram(sys.executable); self.proc.setArguments(args)
        self.proc.readyReadStandardOutput.connect(self.on_worker_out)
        self.proc.readyReadStandardError.connect(lambda: None)
        self.proc.start()
        self.status.setText("generating the note ... (first run downloads the model)")
        self.append("worker: " + " ".join(args[1:5]))

    def on_worker_out(self):
        text = bytes(self.proc.readAllStandardOutput()).decode("utf-8", "replace")
        for line in text.splitlines():
            if not line.startswith("{"):
                continue
            try:
                ev = json.loads(line)
            except json.JSONDecodeError:
                continue
            if ev.get("event") == "status":
                self.status.setText(ev["text"])
            elif ev.get("event") == "error":
                self.append("ERROR " + ev["text"])
            elif ev.get("event") == "done":
                self.audio_path.setText(ev["path"]); self.start.setValue(1.0); self.end.setValue(0.0); self.pitch.setValue(0.0)
                self.append(f"generated {os.path.basename(ev['path'])}")
                self.from_audio()

    # ---------------------------------------------------------------- output
    def play(self):
        if self.table is None:
            return
        import soundfile as sf
        hz = 440.0 * 2 ** ((self.note.currentData() - 69) / 12)
        y = wt.render_preview(self.table, 48000, 5.0, hz, sweep=self.sweep.isChecked())
        path = os.path.join(self.out_dir.text(), "_preview.wav")
        os.makedirs(self.out_dir.text(), exist_ok=True)
        self.player.stop(); self.player.setSource(QUrl())
        sf.write(path, y, 48000)
        self.player.setSource(QUrl.fromLocalFile(path)); self.player.play()

    def do_export(self):
        if self.table is None:
            self.append("nothing to export"); return
        os.makedirs(self.out_dir.text(), exist_ok=True)
        name = wt.slugify(self.name.text() or "table", 60)
        path = os.path.join(self.out_dir.text(), name + ".wav")
        wt.save_table(path, self.table, self.info, float32=self.float32.isChecked())
        self.append(f"exported {path}")
        self.status.setText(f"exported {os.path.basename(path)}")

    def pick_audio(self):
        f, _ = QFileDialog.getOpenFileName(self, "Audio file", os.path.join(ROOT, "Textures"), "Audio (*.wav *.flac *.aif *.aiff *.ogg)")
        if f:
            self.audio_path.setText(f)

    def pick_dir(self):
        d = QFileDialog.getExistingDirectory(self, "Wavetable folder", self.out_dir.text())
        if d:
            self.out_dir.setText(d)

    def append(self, text):
        self.log.appendPlainText(text)


def main():
    app = QApplication(sys.argv)
    w = Main(); w.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
