#!/usr/bin/perl
use IPC::Run qw(run start);
my $localn = shift;
my $remote = shift;

chdir("c00git");
while (1) {
    sleep 1;
    my $fh;
    open $fh, "find .lowered -type d|";
    my %h;
    while (<$fh>) {
	chomp;
	warn;
	if (/^\.lowered\/(.*?)\/([^\/]*)\/([0-9a-f]*)\/([^\/]*)$/) {
	    my ($dir, $base, $sum, $name) = ($1, $2, $3);
	    my $path = $dir . "/" . $base;
	    $h{$path}{$sum}{$name} = [$dir, $base];
	}
    }
    for my $path (keys %h) {
	my $h = $h{$path};
	for my $sum (keys %$h) {
	    my $h = $h->{$sum};
	    my $count = 0;
	    my $dir;
	    my $base;
	    for my $name (keys %$h) {
		$count++;
		$dir = $h->{$name}->[0];
		$base = $h->{$name}->[1];
	    }
	    if ($count == 2) {
		run(["rm", "--", "../upper/$path"]);
	    } else {
		next if ! -e $path;
		my $mysum = `sha512sum $path`;
		$mysum = substr($mysum, 0, 32);
		next if $sum ne $mysum;
		next unless run(["mkdir", "-p", "--", "../lower/$dir"]);
		next unless run(["cp", "--", "../upper/$path", "../lower/$path"]);
		next unless run(["touch", "--", ".lowered/$path/$sum/$localn"]);
		next unless run(["git", "add", "--", ".lowered/$path/$sum/$localn"]);
		next unless run(["git", "commit", "-m", "co-lowered"]);

		eval {
		    my $stdin = "";
		    my $stdout = "";
		    my $stderr = "";
		    run(["ssh", $remote, "date", ">>", "sync/syncfs-pings"],
			\$stdin, \$stdout, \$stderr) or die $stderr;
		};
	    }
	}
    }
}
