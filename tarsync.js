let child_process = require("child_process");
let stream = require("stream");

let remote = "10.4.0.2";
let remote_path = "sync/merge";

function pusher()
{
    let subprocess = child_process.spawn("ssh", [remote, `(cd ${remote_path}; git unpack-objects)`]);
    subprocess.stderr.on('data', data => {
	console.error(data.toString());
    });
    return subprocess.stdin;
}

function packer(shas)
{
    let subprocess = child_process.spawn("git", ["pack-objects", "--stdout", "--all-progress"]);
    let drain = () => {
	while (shas.length && subprocess.stdin.write(shas.shift() + "\n"));
	if (shas.length === 0)
	    subprocess.stdin.end();
    };
    subprocess.stdin.on('drain', drain);
    subprocess.stderr.on('data', data => {
	console.error(data.toString());
    });
    drain();
    return subprocess.stdout;
}

let subprocess =
    child_process.spawn("ssh", [remote, `(cd ${remote_path}; while read; do git cat-file -e "$REPLY" || echo "$REPLY"; done)`]);

function handle_sha(sha)
{
    stream.pipeline(packer([sha]), pusher(), function () {})
    console.error("pipelined", sha);
}

{
    let rbuf = "";
    subprocess.stdout.on('data', data => {
	rbuf += data.toString();
	let m;
	while (m = rbuf.match(/^(.*?)\n/)) {
	    handle_sha(m[1]);
	    rbuf = rbuf.substr(m[1].length + 1);
	}
    });
}

{
    let rbuf = "";
    process.stdin.on('data', data => {
	rbuf += data.toString();
	let m;
	while (m = rbuf.match(/^(.*?)\n/)) {
	    console.error("writing", m[1]);
	    if (!subprocess.stdin.write(m[1] + "\n"))
		break;
	    rbuf = rbuf.substr(m[1].length + 1);
	}
    });
    process.stdin.on('close', () => {
	subprocess.stdin.end();
    });
}
