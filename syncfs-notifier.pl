#!/usr/bin/perl
use IPC::Run qw(run);

my $fifo = shift;
my $fh;
open $fh, $fifo or die;
while (<$fh>) {
    warn;
    my $stdin = "";
    my $stdout = "";
    my $stderr = "";
    run(["ssh", "pip\@10.4.0.1", "/bin/echo", "trigger", ">>", "syncfs-pings"],
	\$stdin, \$stdout, \$stderr) or die $stderr;
}
