"""TextureGen: text-to-audio textures for Noctuary's Texture source slots.

PySide6 GUI over texturegen_worker.py (which runs as a child process so torch never lives
inside a Qt thread). Prompts become WAV files in the Textures folder, named
  <slug>_<model>_<seed>[_<note>].wav
where <note> is the detected base pitch (only for periodic material); the synth reads that
suffix to pitch a texture to the key (Pitch = Note).
"""
import json
import os
import random
import subprocess
import sys

from PySide6.QtCore import QProcess, Qt, QUrl
from PySide6.QtGui import QDesktopServices
from PySide6.QtMultimedia import QAudioOutput, QMediaPlayer
from PySide6.QtWidgets import (QApplication, QCheckBox, QComboBox, QDoubleSpinBox, QFileDialog, QFormLayout, QGroupBox,
                               QHBoxLayout, QLabel, QLineEdit, QListWidget, QListWidgetItem, QMainWindow, QPlainTextEdit,
                               QProgressBar, QPushButton, QSpinBox, QVBoxLayout, QWidget)

HERE = os.path.dirname(os.path.abspath(__file__))
WORKER = os.path.join(HERE, "texturegen_worker.py")
# Where a hand-run experiment lands. NOT Library/: what the three designers write while
# somebody is trying things out is not the shipping library, and three folders in the repo
# root called Textures, Wavetables and Impulses looked exactly like a second one.
DEFAULT_OUT = os.path.normpath(os.path.join(HERE, "..", "..", "Scratch", "Textures"))
sys.path.insert(0, HERE)
from texturegen_worker import MODELS  # noqa: E402

PROMPT_IDEAS = [
    "soft breathy flute overtones, held note, slowly wavering, close and dry",
    "bowed metal plate, long resonant shimmer, glassy overtones, no rhythm",
    "gentle rain on a tin roof at night, distant thunder, wide stereo",
    "deep analog circuit hum with slow filter sweeps, warm, sub bass",
    "tibetan singing bowl, one strike, very long decay, room air",
    "wind through pine trees, high mountain, slow gusts, no birds",
    "choir of low male voices humming a single drone, cathedral",
    "crackling ice sheet, creaks and deep booms, frozen lake",
]


class Main(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Noctuary TextureGen")
        self.resize(980, 720)
        self.proc = None
        self.queue = []
        self.busy = False
        self.player = QMediaPlayer(self)
        self.audio_out = QAudioOutput(self)
        self.player.setAudioOutput(self.audio_out)

        root = QWidget(); self.setCentralWidget(root)
        outer = QHBoxLayout(root)
        left = QVBoxLayout(); outer.addLayout(left, 3)
        right = QVBoxLayout(); outer.addLayout(right, 2)

        box = QGroupBox("Prompt"); left.addWidget(box)
        form = QFormLayout(box)
        self.model = QComboBox()
        for key, (label, hub, sr, mx, note) in MODELS.items():
            self.model.addItem(label, key)
        self.model.currentIndexChanged.connect(self.on_model)
        form.addRow("Model", self.model)
        self.prompt = QPlainTextEdit(); self.prompt.setPlaceholderText("what the texture sounds like ..."); self.prompt.setFixedHeight(70)
        self.prompt.setPlainText(random.choice(PROMPT_IDEAS))
        form.addRow("Prompt", self.prompt)
        self.negative = QLineEdit("music, melody, drums, rhythm, voice, speech")
        form.addRow("Negative", self.negative)
        ideas = QPushButton("Idea"); ideas.clicked.connect(lambda: self.prompt.setPlainText(random.choice(PROMPT_IDEAS)))
        form.addRow("", ideas)

        box = QGroupBox("Render"); left.addWidget(box)
        form = QFormLayout(box)
        self.seconds = QDoubleSpinBox(); self.seconds.setRange(1, 47); self.seconds.setValue(30); self.seconds.setSuffix(" s")
        form.addRow("Length", self.seconds)
        self.steps = QSpinBox(); self.steps.setRange(10, 300); self.steps.setValue(100)
        form.addRow("Steps", self.steps)
        self.guidance = QDoubleSpinBox(); self.guidance.setRange(1, 20); self.guidance.setValue(7.0); self.guidance.setSingleStep(0.5)
        form.addRow("Guidance", self.guidance)
        seedrow = QHBoxLayout()
        self.seed = QSpinBox(); self.seed.setRange(0, 2_000_000_000); self.seed.setValue(random.randint(1, 99999))
        self.random_seed = QCheckBox("random"); self.random_seed.setChecked(True)
        seedrow.addWidget(self.seed); seedrow.addWidget(self.random_seed)
        form.addRow("Seed", seedrow)
        self.count = QSpinBox(); self.count.setRange(1, 32); self.count.setValue(4)
        form.addRow("Variations", self.count)
        outrow = QHBoxLayout()
        self.out_dir = QLineEdit(DEFAULT_OUT)
        browse = QPushButton("..."); browse.setFixedWidth(32); browse.clicked.connect(self.pick_dir)
        openb = QPushButton("Open"); openb.setFixedWidth(50); openb.clicked.connect(lambda: QDesktopServices.openUrl(QUrl.fromLocalFile(self.out_dir.text())))
        outrow.addWidget(self.out_dir); outrow.addWidget(browse); outrow.addWidget(openb)
        form.addRow("Folder", outrow)

        row = QHBoxLayout(); left.addLayout(row)
        self.go = QPushButton("Generate"); self.go.setMinimumHeight(36); self.go.clicked.connect(self.enqueue)
        self.stop = QPushButton("Stop worker"); self.stop.clicked.connect(self.stop_worker)
        row.addWidget(self.go, 2); row.addWidget(self.stop, 1)
        self.progress = QProgressBar(); self.progress.setRange(0, 100); left.addWidget(self.progress)
        self.status = QLabel("idle"); left.addWidget(self.status)
        self.log = QPlainTextEdit(); self.log.setReadOnly(True); self.log.setMaximumBlockCount(400); left.addWidget(self.log, 1)

        right.addWidget(QLabel("Results (double-click to play, right button folder)"))
        self.results = QListWidget(); right.addWidget(self.results, 1)
        self.results.itemDoubleClicked.connect(self.play_item)
        prow = QHBoxLayout(); right.addLayout(prow)
        play = QPushButton("Play"); play.clicked.connect(lambda: self.play_item(self.results.currentItem()))
        stopp = QPushButton("Stop"); stopp.clicked.connect(self.player.stop)
        folder = QPushButton("Folder"); folder.clicked.connect(lambda: QDesktopServices.openUrl(QUrl.fromLocalFile(self.out_dir.text())))
        prow.addWidget(play); prow.addWidget(stopp); prow.addWidget(folder)
        hint = QLabel("In Noctuary: Source 2/3 → Type Texture → Texture... → pick the file. A _A3-style suffix pitches it to the key.")
        hint.setWordWrap(True); right.addWidget(hint)
        self.on_model()
        self.load_existing()

    # ---------------------------------------------------------------- worker
    def ensure_worker(self):
        if self.proc is not None and self.proc.state() != QProcess.NotRunning:
            return True
        self.proc = QProcess(self)
        self.proc.setProgram(sys.executable)
        self.proc.setArguments([WORKER, "--serve"])
        self.proc.setProcessChannelMode(QProcess.SeparateChannels)
        self.proc.readyReadStandardOutput.connect(self.on_stdout)
        self.proc.readyReadStandardError.connect(self.on_stderr)
        self.proc.finished.connect(self.on_finished)
        self.proc.start()
        if not self.proc.waitForStarted(5000):
            self.append("could not start the worker"); return False
        self.append("worker started")
        return True

    def stop_worker(self):
        self.queue.clear()
        if self.proc is not None and self.proc.state() != QProcess.NotRunning:
            self.proc.write(b'{"cmd": "quit"}\n')
            if not self.proc.waitForFinished(3000):
                self.proc.kill()
        self.busy = False
        self.status.setText("worker stopped")

    def on_finished(self):
        self.busy = False
        self.append("worker exited")

    def on_stderr(self):
        text = bytes(self.proc.readAllStandardError()).decode("utf-8", "replace")
        for line in text.splitlines():
            if line.strip() and "it/s" not in line and "%|" not in line:
                self.append("  " + line.strip())

    def on_stdout(self):
        text = bytes(self.proc.readAllStandardOutput()).decode("utf-8", "replace")
        for line in text.splitlines():
            line = line.strip()
            if not line.startswith("{"):
                if line: self.append("  " + line)
                continue
            try:
                ev = json.loads(line)
            except json.JSONDecodeError:
                continue
            kind = ev.get("event")
            if kind == "status":
                self.status.setText(ev["text"]); self.append(ev["text"])
            elif kind == "progress":
                self.progress.setValue(int(100 * ev["step"] / max(1, ev["total"])))
            elif kind == "done":
                self.progress.setValue(100)
                note = f"  [{ev['note']}, {ev['hz']:.1f} Hz]" if ev.get("note") else "  [unpitched]"
                self.append(f"done: {os.path.basename(ev['path'])}  {ev['seconds']:.1f} s in {ev.get('elapsed', 0):.0f} s{note}")
                self.add_result(ev["path"])
                self.busy = False
                self.next_job()
            elif kind == "error":
                self.append("ERROR " + ev["text"]); self.status.setText("error (see log)")
                self.busy = False
                self.next_job()

    # ---------------------------------------------------------------- jobs
    def enqueue(self):
        prompt = self.prompt.toPlainText().strip()
        if not prompt:
            return
        key = self.model.currentData()
        base = random.randint(1, 2_000_000_000) if self.random_seed.isChecked() else self.seed.value()
        for i in range(self.count.value()):
            self.queue.append({"cmd": "generate", "model": key, "prompt": prompt, "negative": self.negative.text().strip(),
                               "seconds": self.seconds.value(), "steps": self.steps.value(), "guidance": self.guidance.value(),
                               "seed": base + i, "out_dir": self.out_dir.text()})
        self.append(f"queued {self.count.value()} x {MODELS[key][0]}")
        self.next_job()

    def next_job(self):
        if self.busy or not self.queue:
            if not self.queue and not self.busy: self.status.setText("idle")
            return
        if not self.ensure_worker():
            return
        job = self.queue.pop(0)
        self.busy = True
        self.progress.setValue(0)
        self.status.setText(f"generating ({len(self.queue)} more queued)")
        self.proc.write((json.dumps(job) + "\n").encode("utf-8"))

    # ---------------------------------------------------------------- results
    def add_result(self, path):
        item = QListWidgetItem(os.path.basename(path)); item.setData(Qt.UserRole, path)
        self.results.insertItem(0, item); self.results.setCurrentItem(item)

    def load_existing(self):
        d = self.out_dir.text()
        if not os.path.isdir(d):
            return
        for name in sorted(os.listdir(d)):
            if name.lower().endswith(".wav"):
                self.add_result(os.path.join(d, name))

    def play_item(self, item):
        if item is None:
            return
        self.player.stop()
        self.player.setSource(QUrl.fromLocalFile(item.data(Qt.UserRole)))
        self.player.play()

    def pick_dir(self):
        d = QFileDialog.getExistingDirectory(self, "Texture folder", self.out_dir.text())
        if d:
            self.out_dir.setText(d); self.results.clear(); self.load_existing()

    def on_model(self):
        key = self.model.currentData()
        label, hub, sr, mx, note = MODELS[key]
        self.seconds.setMaximum(mx)
        if self.seconds.value() > mx: self.seconds.setValue(mx)
        self.steps.setEnabled(not key.startswith("musicgen"))
        self.negative.setEnabled(not key.startswith("musicgen"))
        self.status.setText(f"{hub}, {sr} Hz, up to {mx:.0f} s. {note}")

    def append(self, text):
        self.log.appendPlainText(text)

    def closeEvent(self, e):
        self.stop_worker()
        super().closeEvent(e)


def main():
    app = QApplication(sys.argv)
    w = Main(); w.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
