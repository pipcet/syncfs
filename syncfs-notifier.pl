#!/usr/bin/perl
use AE;
use AnyEvent;
use AnyEvent::Handle;
use IPC::Run qw(run);

my $fifo = shift;
my $remote = shift;
my $fh;
open $fh, $fifo or die;
my $timer;
my $hdl; $hdl = new AnyEvent::Handle(
    fh => $fh,
    on_read => sub {
	shift->unshift_read(line => sub {
	    $timer = AE::timer 1, 0, sub {
		my $stdin = "";
		my $stdout = "";
		my $stderr = "";
		run(["ssh", $remote, "date", ">>", "sync/syncfs-pings"],
		    \$stdin, \$stdout, \$stderr) or die $stderr;
	    };
			    });
    });

AnyEvent->condvar->recv;
