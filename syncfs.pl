#!/usr/bin/perl
package SyncFSFile;

sub new {
    my $class = shift;
    my $path = shift;

    return bless { path => $path, btime => time }, $class;
}

sub touch {
    my ($self, $h, $done) = @_;
    $self->{time} = time;
    if ($h->{command} eq "write" and !$done) {
	$self->{modbytes} += $h->{size} + length $h->{path};
	$self->{edited_by}->{$h->{user}}++;
	my $cmdline = $h->{cmdline}->[0];
	$self->{cmdlines}->{$cmdline} = $cmdline;
    } elsif ($h->{command} eq "write" and $done) {
	$self->{status} = "written";
    } elsif ($h->{command} eq "unlink" and $done) {
	$self->{status} = "deleted";
    } elsif ($h->{command} eq "unlink" and !$done) {
	delete $self->{status};
    } elsif ($h->{command} eq "rename" and !$done) {
	delete $self->{status};
    }
}

sub touch_renamed_to {
    my ($self, $h, $done) = @_;
    $self->{time} = time;
    if (!$done) {
	delete $self->{status};
    } else {
	$self->{status} = written;
    }
}

sub score {
    my ($self) = @_;
    my $age = time - $self->{time};
    my $modbytes = (1 + $self->{modbytes});
    my $write_time = $self->{time} - $self->{btime};

    return ($age + 1) * $modbytes;
}

sub path {
    my $self = shift;
    return $self->{path};
}

sub message {
    my $self = shift;
    $self->{message} .= "automatic commit\n\n";
    for my $cmdline (sort %{$self->{cmdlines}}) {
	$self->{message} .= $self->path . " " . "$cmdline\n";
    }
    $self->{cmdlines} = {};
    my $message = $self->{message};
    $self->{message} = "";
    return $message;
}

sub sync {
    my $self = shift;
    $self->{modbytes} = 0;
}

package main;

use strict;
use Mojo::JSON;
use File::Slurp qw(read_file);
use Data::Dumper;
use User::pwent;
use IPC::Run qw(run start);
use AnyEvent;
use AnyEvent::Handle;
use Carp::Always;

my $fifo = shift;
my $outfifo = shift;
my $notifyfifo = shift;

my %files;

sub sync {
    my $stdin = join("\0", map { $_->path } sort { $b <=> $a } values %files);
    my $stdout = "";
    my $stderr = "";
    run(["echo", "rsync", "--update", "--files-from=-", "--from0", "mount", "pip\@10.4.0.1:syncfs/"], \$stdin, \$stdout, \$stderr);
    warn "stdout $stdout";
    warn "stderr $stderr";
}

sub del_files {
    my $stdin = "";
    my $maxfiles = 1024;
    my $i = 0;
    my @files;
    my $message = "";
    for my $file (sort { $b->score <=> $a->score } values %files) {
	next if $file->{status} ne "deleted";
	last if ($i++ == $maxfiles);
	push @files, $file;
	$message .= $file->message;
    }

    if (@files) {
	my $stdin = join("\0", map { $_->path } @files);
	eval {
	    run(["git", "rm", "--ignore-unmatch", "--pathspec-from-file=-", "--pathspec-file-nul"], \$stdin) or die;
	    run(["git", "commit", "--allow-empty", "-m", $message]) or die;
	    for my $file (@files) {
		$file->sync;
	    }
	};
    }
}

sub add_files {
    my $stdin = "";
    my $maxfiles = 1024;
    my $i = 0;
    my @files;
    my $message = "";
    for my $file (sort { $b->score <=> $a->score } values %files) {
	next if $file->{status} ne "written";
	last if $file->score == 0;
	last if ($i++ == $maxfiles);
	push @files, $file;
	$message .= $file->message;
    }

    if (@files) {
	my $stdin = join("\0", map { $_->path } @files);
	run(["git", "add", "--ignore-removal", "--pathspec-from-file=-", "--pathspec-file-nul"], \$stdin) or die;
	run(["git", "commit", "--allow-empty", "-m", $message]) or die;
	for my $file (@files) {
	    $file->sync;
	}
    }
}

my $resolve_starid_last_pid;
my $resolve_starid_user;
my $resolve_starid_cmdline;
sub resolve_starid {
    my ($h) = @_;
    if ($h->{pid} eq $resolve_starid_last_pid) {
	$h->{cmdline} = $resolve_starid_cmdline;
	$h->{user} = $resolve_starid_user;
    }
    $resolve_starid_last_pid = $h->{pid};
    eval {
	my @cmdline = split("\0", read_file("/proc/" . $h->{pid} . "/cmdline"));
	$resolve_starid_cmdline = $h->{cmdline} = \@cmdline;
	$resolve_starid_user = $h->{user} = getpwuid($h->{uid})->name;
    };
}

sub update_score {
    my ($h, $done) = @_;
    if (!exists $files{$h->{path}}) {
	$files{$h->{path}} = new SyncFSFile($h->{path});
    }
    my $file = $files{$h->{path}};
    $file->touch($h, $done);
}

sub update_rename_score {
    my ($h, $done) = @_;
    if (!exists $files{$h->{path1}}) {
	$files{$h->{path1}} = new SyncFSFile($h->{path1});
    }
    if (!exists $files{$h->{path2}}) {
	$files{$h->{path2}} = new SyncFSFile($h->{path2});
    }
    {
	my $file = $files{$h->{path1}};
	$file->touch($h, $done);
    }
    {
	my $file = $files{$h->{path2}};
	$file->touch_renamed_to($h, $done);
    }
}

sub contact_sync_host {
    my ($host) = @_;
    my $stdout = "";
    my $stderr = "";
    my $stdin = "";
    run(["ssh", "$host", "echo", "foo", ">>", "syncfs/syncfs-pings"], \$stdin, \$stdout, \$stderr);
}

my $synctime = 0;

my $fh;
open $fh, "$fifo" or last;
my $outfh;
open $outfh, ">$outfifo" or die;
my $notifyfh;
open $notifyfh, ">$notifyfifo" or die;
chdir "lower";

my $timer_last_run = 0;
my $timer_running = 0;

sub run_timer {
    $timer_last_run = time;
    return if ($timer_running);
    $timer_running = 1;
    warn "running timer";
    add_files();
    del_files();
    # contact_sync_host("10.4.0.1");
    print $notifyfh `pwd`;
    flush $notifyfh;
    $timer_running = 0;
    $timer_last_run = time;
}

my $timer_last_started = 0;
my $timer;
sub check_timer;
sub check_timer {
    my $time = time;
    if ($time - $timer_last_started > 5) {
	run_timer;
    } elsif ($time - $timer_last_run > 60) {
	run_timer;
    }
    $timer = AE::timer 5, 0, sub { check_timer };
}

my $cv = AnyEvent->condvar();
my $hdl; $hdl = new AnyEvent::Handle(
    fh => $fh,
    on_read => sub {
	my $hash;
	shift->unshift_read(line => sub {
	    my ($h, $line) = @_;
	    #print $line . "\n";
	    if ($line ne "") {
		$hash = Mojo::JSON::decode_json($line);
		resolve_starid($hash);
		$outfh->print("\n");
		$outfh->flush;
		if ($hash->{command} eq "rename") {
		    update_rename_score($hash, 0);
		} else {
		    update_score($hash, 0);
		}
	    } else {
		if ($hash->{command} eq "rename") {
		    update_rename_score($hash, 1);
		} else {
		    update_score($hash, 1);
		}
	    }

	    check_timer;
	    $timer = AE::timer 5, 0, sub {
		check_timer;
	    }
			    });
    },
    on_eof => sub { $cv->send },
    on_error => sub { $cv->send });

warn "starting";
$cv->recv();
warn "exiting";
