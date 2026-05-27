"""
CSV logging utilities for publishable experiment runs.
"""

from __future__ import annotations

import csv
from dataclasses import asdict
from pathlib import Path
from typing import Optional


class CSVRunLogger:
    def __init__(self, path: str):
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._fh = self.path.open("w", newline="", encoding="utf-8")
        self._writer: Optional[csv.DictWriter] = None

    def log_row(self, row: dict) -> None:
        if self._writer is None:
            self._writer = csv.DictWriter(self._fh, fieldnames=list(row.keys()))
            self._writer.writeheader()
        self._writer.writerow(row)
        self._fh.flush()

    def close(self) -> None:
        try:
            self._fh.close()
        except Exception:
            pass

