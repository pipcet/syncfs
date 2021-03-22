#!/usr/bin/perl
use IPC::Run qw(run start);
use File::Slurp qw(read_file write_file);

my $fifo;
open $fifo, "fifos/daemon-to-lowerer";

my $localn = shift;
my $remote = shift;

chdir "c00git";
while (<$fifo>) {
    chomp;
    next; # automatic lowering is disabled because overlayfs is buggy.
    my $path = $_;
    next unless -e $path;
    my $rev = `git rev-parse HEAD`;
    my $rrev = `git rev-parse $remote/main`;
    my $contents = read_file($path);
    my $stdin = "";
    my $stdout = "";
    next unless run(["git", "diff", "HEAD..main", "--", $path],
		    \$stdin, \$stdout);
    next unless run(["git", "diff", "HEAD..$remote/main", "--", $path],
		    \$stdin, \$stdout);
    next unless run(["git", "diff", "HEAD..main", "--", $path],
		    \$stdin, \$stdout);
    my $sum = `sha512sum $path`;
    $sum = substr($sum, 0, 32);
    next unless `git rev-parse HEAD` eq $rev;
    warn "lowering $path...";
    system("cp -- ../upper/$path ../lower/$path");
    system("mkdir -p .lowered/$path/$sum && touch .lowered/$path/$sum/$localn && git add .lowered/$path/$sum/$localn");
    system("git commit -m lowered");
}
