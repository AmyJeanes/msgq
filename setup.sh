#!/usr/bin/env bash
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null && pwd)"
cd $DIR

if ! command -v uv &>/dev/null; then
  echo "'uv' is not installed. Installing 'uv'..."
  curl -LsSf https://astral.sh/uv/install.sh | sh

  # doesn't require sourcing on all platforms
  set +e
  source $HOME/.local/bin/env
  set -e
fi

VENV_BIN=bin
EXE=""
case "$(uname -s)" in MINGW*|MSYS*)
  VENV_BIN=Scripts  # a native Python's venv keeps its scripts in Scripts/
  EXE=".exe"
  export UV_PYTHON="${UV_PYTHON:-3.12}"  # not the MSYS2 toolchain's own python, whose wheels are incompatible
  ;;
esac

export UV_PROJECT_ENVIRONMENT="$DIR/.venv"
uv sync --all-extras
source "$DIR/.venv/$VENV_BIN/activate"
