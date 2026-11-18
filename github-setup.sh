#!/bin/sh
# One-time: puts this project on GitHub so GitHub Actions can build the Windows (and Mac) versions.
#   sh github-setup.sh https://github.com/<you>/threes-company.git
# Before running it: create an EMPTY private repository on github.com (no README, no licence),
# and sign Git in to GitHub on this Mac (the GitHub CLI does it: brew install gh && gh auth login).
set -e
cd "$(dirname "$0")"
URL="$1"
[ -n "$URL" ] || { echo "Usage: sh github-setup.sh https://github.com/<you>/threes-company.git"; exit 1; }
command -v git >/dev/null 2>&1 || { echo "Git is missing: run xcode-select --install"; exit 1; }

# the build recipe for GitHub Actions lives in .github/workflows (kept beside the project as github-workflow-build.yml)
mkdir -p .github/workflows
[ -f github-workflow-build.yml ] && cp github-workflow-build.yml .github/workflows/build.yml

if [ ! -d .git ]; then
  git init
  git symbolic-ref HEAD refs/heads/main   # works on old and new Git alike
fi
git config user.name >/dev/null 2>&1 || git config user.name "Otis Kanno"
git config user.email >/dev/null 2>&1 || git config user.email "noreply@users.noreply.github.com"
# make the private repository on GitHub if it isn't there yet (needs gh auth login done)
if command -v gh >/dev/null 2>&1; then
  REPO=$(echo "$URL" | sed -E 's#^https://github.com/##; s#\.git$##')
  gh repo view "$REPO" >/dev/null 2>&1 || gh repo create "$REPO" --private
fi
git add -A
git commit -m "Three's Company 0.18" || echo "(nothing new to commit)"
git remote remove origin >/dev/null 2>&1 || true
git remote add origin "$URL"
git branch -M main
git push -u origin main
echo ""
echo "Pushed. GitHub is now building: open $URL/actions"
echo "When the run is green, the Windows and Mac zips are under the run's Artifacts."
