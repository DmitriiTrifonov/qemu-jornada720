#!/bin/sh
# Install a local FrogFind (https://github.com/ActionRetro/FrogFind), the
# search engine and page simplifier for vintage browsers, for run.sh to
# serve to CE at http://10.0.2.2:8720/ (Pocket IE cannot do modern HTTPS;
# FrogFind fetches pages itself and hands them over as plain HTML).
#
# Needs: git, composer and PHP with dom, xml, mbstring, openssl, gd,
# intl (Alpine: apk add composer php85-dom php85-xml php85-gd php85-ctype
# php85-intl). FrogFind is GPL-3.0 and lives outside this repository:
#   ${XDG_DATA_HOME:-~/.local/share}/qemu-jornada720/frogfind
# Run again to update it.
set -e

DIR=${XDG_DATA_HOME:-$HOME/.local/share}/qemu-jornada720/frogfind

if [ -d "$DIR/.git" ]; then
    git -C "$DIR" pull --ff-only
else
    mkdir -p "$(dirname "$DIR")"
    git clone --depth 1 https://github.com/ActionRetro/FrogFind.git "$DIR"
fi
cd "$DIR"
composer install --no-dev --no-interaction --no-progress
echo "FrogFind installed in $DIR; run.sh will serve it at http://10.0.2.2:8720/"
