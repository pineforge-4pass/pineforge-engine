"""Tests for resolving the release image's default command from Docker inspect."""

import json
from pathlib import Path
import subprocess
import sys
from unittest import mock

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import pf_release_run


IMAGE = "pineforge-release:test"
IMAGE_ID = "sha256:" + "a" * 64


def _inspect_with_config(config):
    result = subprocess.CompletedProcess(
        ["docker", "image", "inspect", IMAGE], 0,
        stdout=json.dumps([{"Id": IMAGE_ID, "Config": config}]), stderr="",
    )
    return mock.patch.object(pf_release_run.subprocess, "run", return_value=result)


@pytest.mark.parametrize("field", ["Entrypoint", "Cmd"])
def test_image_command_accepts_one_command_field(field):
    command = ["/entrypoint.sh"]
    with _inspect_with_config({field: command}):
        assert pf_release_run._image_command(IMAGE) == (IMAGE_ID, command)


def test_image_command_refuses_a_config_without_command_fields():
    with _inspect_with_config({}):
        with pytest.raises(pf_release_run.ReleaseRunError, match="image has no default command"):
            pf_release_run._image_command(IMAGE)
