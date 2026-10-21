#!/bin/sh
# Commit the staged changes as the configured git user, with exactly the given message.
#
#   git add <paths>; tools/commit.sh "Subject" ["Body"]
#
# Uses plumbing (write-tree / commit-tree / update-ref) so tools that append trailers to
# `git commit` messages (for example "Co-authored-by: ...") cannot touch the message: the repository
# owner is the sole author of every commit. Refuses messages that contain such a trailer.
set -eu
[ $# -ge 1 ] || { echo "usage: $0 \"Subject\" [\"Body\"]" >&2; exit 2; }

msg=$1
[ $# -ge 2 ] && msg=$(printf '%s\n\n%s' "$1" "$2")
if printf '%s' "$msg" | grep -qiE '^(co-authored-by|signed-off-by):'; then
	echo "commit.sh: message contains an authorship trailer; the owner is the sole author" >&2
	exit 1
fi

git diff --cached --quiet && { echo "commit.sh: nothing staged" >&2; exit 1; }
name=$(git config user.name) && email=$(git config user.email) || { echo "commit.sh: set git user.name/user.email" >&2; exit 1; }
export GIT_AUTHOR_NAME=$name GIT_AUTHOR_EMAIL=$email GIT_COMMITTER_NAME=$name GIT_COMMITTER_EMAIL=$email

branch=$(git symbolic-ref -q HEAD) || { echo "commit.sh: detached HEAD" >&2; exit 1; }
tree=$(git write-tree)
if parent=$(git rev-parse -q --verify HEAD 2>/dev/null); then
	new=$(printf '%s\n' "$msg" | git commit-tree "$tree" -p "$parent")
else
	new=$(printf '%s\n' "$msg" | git commit-tree "$tree")
fi
git update-ref -m "commit: $1" "$branch" "$new"
git log -1 --format='committed %h by %an <%ae>: %s'
