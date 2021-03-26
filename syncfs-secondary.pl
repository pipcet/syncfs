#!/usr/bin/perl
use IPC::Run qw(run);
use AnyEvent;
use AE;
use AnyEvent::Handle;

my $remote = shift;

sub handle_line {
    run(["sh", "-c", "cd c00git; git pull $remote main --no-edit; git commit -m merge"]);
}

my $fh2;
open $fh2, "remote-to-local" or die;

my $hdl2; $hdl2 = new AnyEvent::Handle(
    fh => $fh2,
    on_read => sub {
	shift->unshift_read(line => sub {
	    my ($h, $line) = @_;
	    handle_line($line);
			    });
    });

AnyEvent->condvar->recv;
