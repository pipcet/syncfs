#!/usr/bin/perl
package SyncFSFile;

use Data::Dumper;

sub bucket {
    my ($n) = shift;
    if ($n == 0) {
	return 0;
    }
    if ($n < 0) {
	return - bucket(-$n);
    }
    my $bucket = 0;
    while ($n) {
	$bucket++;
	$n = int($n/2);
    }
    return $bucket;
}

sub status_time_stats {
    my $time0 = time;
    for my $status (sort { $a cmp $b } keys %SyncFSFile::by_status_time) {
	my $h = $SyncFSFile::by_status_time{$status};
	my %buckets;
	for my $time (sort { $a <=> $b } keys %$h) {
	    my $count = 0;
	    my $h = $h->{$time};
	    for my $file (values %$h) {
		$buckets{bucket($time0 - $time)}++;
	    }
	}
	for my $bucket (sort { $a <=> $b } keys %buckets) {
	    print "$status $bucket " . $buckets{$bucket} . "\n";
	}
    }
}

sub new {
    my $class = shift;
    my $path = shift;
    my $time = time;

    return bless {
	path => $path,
	btime => $time,
	old_by_time => $time,
	status => "unknown"
    }, $class;
}

sub status {
    my ($self, $newstatus) = @_;
    return $self->{status} unless defined $newstatus;

    my $status = $self->{status};
    my $time = $self->{old_by_time};
    delete $SyncFSFile::by_status_time{$status}{$time}{$self};
    if (scalar keys %{$SyncFSFile::by_status_time{$status}{$time}} == 0) {
	delete $SyncFSFile::by_status_time{$status}{$time};
    }

    $self->{status} = $newstatus;

    $SyncFSFile::by_status_time{$newstatus}{$time}{$self} = $self;

    return $newstatus;
}

sub old_by_time {
    my ($self, $newtime) = @_;
    return $self->{old_by_time} unless defined $newtime;

    my $status = $self->{status};
    my $time = $self->{old_by_time};
    delete $SyncFSFile::by_status_time{$status}{$time}{$self};
    if (scalar keys %{$SyncFSFile::by_status_time{$status}{$time}} == 0) {
	delete $SyncFSFile::by_status_time{$status}{$time};
    }

    $self->{old_by_time} = $newtime;

    $SyncFSFile::by_status_time{$status}{$newtime}{$self} = $self;

    return $newtime;
}

sub touch {
    my ($self, $h, $done, $delay) = @_;
    $self->{time} = time;
    if ($h->{command} eq "write" and !$done) {
	$self->{modbytes} += $h->{size} + length $h->{path};
	$self->{edited_by}->{$h->{user}}++;
	my $cmdline = $h->{cmdline}->[0];
	$self->{cmdlines}->{$cmdline} = $cmdline;
    } elsif (($h->{command} eq "write") and $done) {
	$self->status("written");
	$self->old_by_time(time + $delay);
    } elsif ($h->{command} eq "symlink" and !$done) {
	$self->{modbytes} += $h->{size} + length $h->{path};
	$self->{edited_by}->{$h->{user}}++;
	my $cmdline = $h->{cmdline}->[0];
	$self->{cmdlines}->{$cmdline} = $cmdline;
    } elsif (($h->{command} eq "symlink") and $done) {
	$self->status("written");
	$self->old_by_time(time + $delay);
    } elsif (($h->{command} eq "create") and $done) {
	$self->status("written");
	$self->old_by_time(time + $delay);
    } elsif (($h->{command} eq "unlink") and $done) {
	$self->status("deleted");
	$self->old_by_time(time + $delay);
    } elsif (($h->{command} eq "unlink") and !$done) {
	$self->status("unknown");
    } elsif (($h->{command} eq "rename") and !$done) {
	$self->status("unknown");
    }
}

sub touch_renamed_to {
    my ($self, $h, $done) = @_;
    $self->{time} = time;
    if (!$done) {
	$self->status("unknown");
    } else {
	$self->status("written");
    }
}

sub touch_renamed_from {
    my ($self, $h, $done) = @_;
    $self->{time} = time;
    if (!$done) {
	$self->status("unknown");
    } else {
	$self->status("deleted");
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
    $self->status("synched");
    $self->old_by_time(time + 120);
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

my $timer_needed = 0;
my $fifos = shift;

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
	next if $file->status ne "deleted";
	if ($i++ == $maxfiles) {
	    $timer_needed = 1;
	    last;
	}
	push @files, $file;
	$message .= $file->message;
    }

    if (@files) {
	my $stdin = join("\0", map { $_->path } @files) . "\0";
	eval {
	    my $succ = run(["git", "rm", "--ignore-unmatch", "--pathspec-from-file=-", "--pathspec-file-nul"], \$stdin);
	    if ($succ) {
		$succ = run(["git", "commit", "--allow-empty", "-m", substr($message, 0, 1024)]);
	    }
	    for my $file (@files) {
		$file->sync;
	    }
	};

	warn $@ if $@;

	return 1;
    }

    return 0;
}

sub add_files {
    my %opts = @_;
    my $stdin = "";
    my $maxfiles = 1024;
    my $i = 0;
    my @files;
    my $message = "";
    for my $file (sort { $b->score <=> $a->score } values %files) {
	next if $file->status ne "written";
	next if $opts{nolarge} and $file->{modbytes} > 1024 * 1024;
	last if $file->score == 0;
	if ($i++ == $maxfiles) {
	    $timer_needed = 1;
	    last;
	}
	push @files, $file;
	$message .= $file->message;
    }

    if (@files) {
	my $stdin = join("\0", map { $_->path } @files) . "\0";
	my $succ = run(["git", "add", "--ignore-removal", "--pathspec-from-file=-", "--pathspec-file-nul"], \$stdin);
	if ($succ) {
	    $succ = run(["git", "commit", "--allow-empty", "-m", $message]);
	}
	my $rev = "";
	if ($succ) {
	    $stdin = "";
	    $succ = run(["git", "rev-parse", "HEAD"], \$stdin, \$rev);
	    chomp $rev;
	}
	if (!$succ) {
	    die;
	}
	for my $file (@files) {
	    $file->sync;
	}

	return 1;
    }

    return 0;
}

my @lowerer_queue;
my $lowerer_handle;
my $lowerer_fh;

sub lower_files {
    return 0; # disabled; overlayfs is buggy
    my %opts = @_;
    my $maxfiles = 32;
    my @files;
    my $i = 0;
    for my $file (sort { $b->score <=> $a->score } values %files) {
	next if $file->status ne "synched";
	if ($i++ == $maxfiles) {
	    $timer_needed = 1;
	    last;
	}
	push @files, $file;
    }

    if (@files) {
	for my $file (@files) {
	    if (@lowerer_queue == 0) {
		warn "lowerer_queue now " . scalar(@lowerer_queue);
		push @lowerer_queue, $file;
		$lowerer_handle = new AnyEvent::Handle(
		    fh => $lowerer_fh,);
		$lowerer_handle->push_write($file->path . "\n");
	    } else {
		push @lowerer_queue, $file;
	    }
	}

	return 1;
    }

    return 0;
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

my %editors = (
    "emacs",
    "vi",
    "vim",
    );

sub update_score {
    my ($h, $done) = @_;
    if (!exists $files{$h->{path}}) {
	$files{$h->{path}} = new SyncFSFile($h->{path});
    }
    my $file = $files{$h->{path}};
    my $delay = 5;
    for my $cmdline (values %{$file->{cmdlines}}) {
	for my $cmd (split "/", $cmdline) {
	    $delay = 0 if $editors{$cmd};
	}
    }
    $file->touch($h, $done, $delay);
    return $delay;
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
	$file->touch_renamed_from($h, $done);
    }
    {
	my $file = $files{$h->{path2}};
	$file->touch_renamed_to($h, $done);
    }

    return 5;
}

sub contact_sync_host {
    my ($host) = @_;
    my $stdout = "";
    my $stderr = "";
    my $stdin = "";
    run(["ssh", "$host", "echo", "foo", ">>", "sync/syncfs-pings"], \$stdin, \$stdout, \$stderr);
}

my $synctime = 0;

my $fh;
open $fh, "$fifos/fuse-to-daemon" or die;
my $outfh;
open $outfh, ">$fifos/daemon-to-fuse" or die;
my $notifyfh;
open $notifyfh, ">$fifos/daemon-to-notify" or die;
open $lowerer_fh, ">$fifos/daemon-to-lowerer" or die;
chdir "c00git";

my $timer_last_run = 0;
my $timer_running = 0;

sub run_timer {
    my %opts = @_;
    # SyncFSFile::status_time_stats;
    $timer_last_run = time;
    return if ($timer_running);
    $timer_running = 1;
    eval {
	my $rev;
	if (($rev = add_files(%opts)) ||
	    ($rev = del_files(%opts))
	    # || ($rev = lower_files(%opts))
	    ) {
	    print $notifyfh $rev . "\n";
	    flush $notifyfh;
	}
    };
    $timer_running = 0;
    $timer_last_run = time;
}

my $timer_last_started = 0;
my $timer;
sub check_timer;
sub check_timer {
    my $time = time;
    if ($time >= $timer_last_started + 5) {
	$timer_needed = 0;
	$timer_last_started = $time;
	run_timer;
    } elsif ($time - $timer_last_run > 60) {
	$timer_needed = 0;
	$timer_last_started = $time;
	run_timer(nolarge => 1);
    }
    $timer = AE::timer 5, 0, sub { check_timer };
    $timer_last_started = $time;
}

my $cv = AnyEvent->condvar();
my $hash;
my $hdl; $hdl = new AnyEvent::Handle(
    fh => $fh,
    on_read => sub {
	shift->unshift_read(line => sub {
	    my ($h, $line) = @_;
	    my $delay;
	    warn "$line\n" if $line ne "";
	    if ($line ne "") {
		$hash = Mojo::JSON::decode_json($line);
		resolve_starid($hash);
		$outfh->print("\n");
		$outfh->flush;
		if ($hash->{command} eq "rename") {
		    $delay = update_rename_score($hash, 0);
		} else {
		    $delay = update_score($hash, 0);
		}
	    } else {
		$outfh->print("\n");
		$outfh->flush;
		if ($hash->{command} eq "rename") {
		    $delay = update_rename_score($hash, 1);
		} else {
		    $delay = update_score($hash, 1);
		}
	    }

	    if ($delay == 0) {
		run_timer(nolarge => 1);
		$timer = AE::timer 5, 0, sub {
		    check_timer;
		};
		$timer_last_started = time;
	    } else {
		check_timer;
		$timer = AE::timer 5, 0, sub {
		    check_timer;
		};
		$timer_last_started = time;
	    }
			    });
    },
    on_eof => sub { $cv->send },
    on_error => sub { $cv->send });

warn "starting";
$cv->recv();
warn "exiting";
