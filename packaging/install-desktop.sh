#!/usr/bin/env bash
# Додає P2P Месенджер у меню застосунків (іконка, запуск кліком).
#   ./packaging/install-desktop.sh            встановити ярлик
#   ./packaging/install-desktop.sh --remove   видалити ярлик
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GUI="$ROOT/build/p2p-gui"
DATA="${XDG_DATA_HOME:-$HOME/.local/share}"
APPS="$DATA/applications"
ICONS="$DATA/icons/hicolor/scalable/apps"

if [[ "${1:-}" == "--remove" ]]; then
    rm -f "$APPS/p2p-messenger.desktop" "$ICONS/p2p-messenger.svg"
    update-desktop-database "$APPS" 2>/dev/null || true
    echo "Ярлик видалено."
    exit 0
fi

if [[ ! -x "$GUI" ]]; then
    echo "Не знайдено $GUI. Спочатку зберіть проєкт: cmake --build build" >&2
    exit 1
fi

mkdir -p "$APPS" "$ICONS"
install -m 644 "$ROOT/packaging/p2p-messenger.svg" "$ICONS/p2p-messenger.svg"

cat > "$APPS/p2p-messenger.desktop" << DESKTOP
[Desktop Entry]
Type=Application
Name=P2P Месенджер
GenericName=Месенджер
Comment=Децентралізований месенджер для локальної мережі
Exec="$GUI" 9000
Icon=p2p-messenger
Terminal=false
Categories=Network;InstantMessaging;Chat;
Keywords=messenger;p2p;chat;месенджер;
StartupNotify=true
DESKTOP
chmod 644 "$APPS/p2p-messenger.desktop"

update-desktop-database "$APPS" 2>/dev/null || true
gtk-update-icon-cache -q -t "$DATA/icons/hicolor" 2>/dev/null || true

echo "Готово. Знайдіть «P2P Месенджер» у меню застосунків (клавіша Super, потім назва)."
echo "Ярлик запускає: $GUI 9000"
