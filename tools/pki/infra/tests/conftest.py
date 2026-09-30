"""pytest bootstrap: put the Lambda layer on sys.path so tests import the
exact module that ships to AWS."""

import os
import sys

LAYER_PYTHON = os.path.join(
    os.path.dirname(__file__), "..", "lambda", "layer", "python"
)
sys.path.insert(0, os.path.abspath(LAYER_PYTHON))
