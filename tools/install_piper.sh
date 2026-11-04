#!/usr/bin/env bash
# 下载 piper CLI（自带 onnxruntime / piper-phonemize / espeak-ng-data）和语音模型。
# 仅用于开发机（x86_64 / aarch64）；开发板请按 README 使用 paroli-server。
set -euo pipefail

PIPER_VERSION="${PIPER_VERSION:-2023.11.14-2}"
DEST="${PIPER_ROOT:-${HOME}/.local/share/tts}"
VOICE="${1:-zh_CN-huayan-medium}"

case "$(uname -m)" in
  x86_64) ARCH=x86_64 ;;
  aarch64|arm64) ARCH=aarch64 ;;
  *)
    echo "unsupported architecture: $(uname -m)" >&2
    exit 1
    ;;
esac

# 语音 ID 形如 zh_CN-huayan-medium：
#   language=zh_CN speaker=huayan quality=medium
LANGUAGE="${VOICE%%-*}"
REST="${VOICE#*-}"
SPEAKER="${REST%-*}"
QUALITY="${REST##*-}"
FAMILY="${LANGUAGE%%_*}"
VOICE_URL="https://huggingface.co/rhasspy/piper-voices/resolve/main/${FAMILY}/${LANGUAGE}/${SPEAKER}/${QUALITY}/${VOICE}.onnx"

mkdir -p "${DEST}/piper" "${DEST}/voices"

if [ ! -x "${DEST}/piper/piper/piper" ]; then
  echo "Downloading piper ${PIPER_VERSION} (${ARCH})"
  TARBALL="$(mktemp -t piper.XXXXXX.tar.gz)"
  trap 'rm -f "${TARBALL}"' EXIT
  curl -fL --retry 3 -o "${TARBALL}" \
    "https://github.com/rhasspy/piper/releases/download/${PIPER_VERSION}/piper_linux_${ARCH}.tar.gz"
  tar xzf "${TARBALL}" -C "${DEST}/piper"
  chmod +x "${DEST}/piper/piper/piper" "${DEST}/piper/piper/piper_phonemize" \
    "${DEST}/piper/piper/espeak-ng"
else
  echo "piper already installed at ${DEST}/piper/piper/piper"
fi

download_voice_file() {
  local file="$1"
  local url="$2"
  if [ -f "${DEST}/voices/${file}" ]; then
    echo "voice file already present: ${DEST}/voices/${file}"
    return
  fi
  echo "Downloading ${file}"
  curl -fL --retry 3 -o "${DEST}/voices/${file}" "${url}"
}

download_voice_file "${VOICE}.onnx" "${VOICE_URL}"
download_voice_file "${VOICE}.onnx.json" "${VOICE_URL}.json"

echo
echo "piper:  ${DEST}/piper/piper/piper"
echo "model:  ${DEST}/voices/${VOICE}.onnx"
echo "Update piper.executable / piper.model if PIPER_ROOT is not ~/.local/share/tts."
