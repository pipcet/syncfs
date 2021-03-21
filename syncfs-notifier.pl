#!/usr/bin/perl
use IPC::Run qw(run);

my $fifo = shift;
my $remote = shift;
my $fh;
open $fh, $fifo or die;
while (<$fh>) {
    warn;
    my $stdin = "";
    my $stdout = "";
    my $stderr = "";
    run(["ssh", $remote, "date", ">>", "sync/syncfs-pings"],
	\$stdin, \$stdout, \$stderr) or die $stderr;
}
