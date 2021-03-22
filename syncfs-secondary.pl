#!/usr/bin/perl
use IPC::Run qw(run);

my $remote = shift;
while(1) {
    {
	my $stdin = "";
	my $stdout = "";
	my $stderr = "";
	run(["inotifywait", "./syncfs-pings"], \$stdin);
	warn $stderr if $stderr;
    }
    {
	my $stdin = "";
	my $stdout = "";
	my $stderr = "";
	run(["sh", "-c", "cd c00git; git pull $remote main"], \$stdin);
	warn $stderr if $stderr;
    }
}
