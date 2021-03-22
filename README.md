file systems:

work - base fs - overlay work directory
lower - base fs - the common, agreed-upon base (for all servers)
upper - base fs - per-server overlay directory
merge - overlay - upper overlaid on lower
c00git - c00gitfs - de-mknodded version of upper
mount - syncfs - mountable version of merge

for synchronization, changes are made in mount. They're written back
to merge, then committed to merge's .git directory. Then they're
shared by git pull and land in other servers' merge .git
directory. Then they're merged and end up in other servers' merge
directories. When all servers agree about a file in upper/, it's moved
to lower/, and removed from the git merge directory. Then merge's .git
history is squashed so we no longer keep a record about the
newly-lowered file.

* TODO change notification format to be either a commit or a commit->commit rewrite
* TODO handle commit->commit rewrites somehow
* TODO find Emacs auto-revert-mode bug. Does it still hap
