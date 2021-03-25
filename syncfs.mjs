import * as oldfs from "fs";
import * as fs from "fs/promises";
import * as readline from "readline";
import * as child_process from "child_process";
import { setTimeout, clearTimeout } from "timers";

let [fifopath, ...remotes] = process.argv.slice(2);

function SyncFSFile(path, event)
{
    this.path = path;
    this.time = Date.now() / 1000.0;
    this.old_by_time = this.time;
    this.set_state("unknown");
    this.factor = 1.0;
    if (path.match(/tmp/))
	this.factor *= .1;
    if (path.match(/#/))
	this.factor *= .1;
    if (path.match(/~$/))
	this.factor *= .1;
    if (event.cmdline[0].match(/emacs/))
	this.factor *= 10:
    this.score = 0.0;
}

SyncFSFile.map_by_path = new Map();
SyncFSFile.by_path = function (path, event) {
    if (!SyncFSFile.map_by_path.has(path))
	SyncFSFile.map_by_path.set(path, new SyncFSFile(path, event));
    return SyncFSFile.map_by_path.get(path);
};

SyncFSFile.map_by_state = new Map();
SyncFSFile.map_by_state.set(undefined, new Set());
SyncFSFile.map_by_state.set("written", new Set());
SyncFSFile.map_by_state.set("deleted", new Set());
SyncFSFile.map_by_state.set("unknown", new Set());
SyncFSFile.map_by_state.set("synched", new Set());

SyncFSFile.prototype.map_by_state = SyncFSFile.map_by_state;

SyncFSFile.by_state = function (state, n)
{
    let files = [...this.map_by_state.get(state)];
    let time = Date.now()/1000.0;
    files.map(file => file.update_score(time));
    files = files.sort((a, b) => b.score - a.score);
    files.splice(n);
    for (let file of files) {
	console.log(file.path, file.score);
    }
    return files;
};

SyncFSFile.prototype.update_score = function (time = Date.now() / 1000.0)
{
    let dt = time - this.time;
    this.score *= Math.exp(dt * .01);
    this.time = time;
};

SyncFSFile.prototype.bump_score = function (event, i)
{
    this.update_score();
    let absscore = this.path.length + event.size;
    let relscore = 1 - Math.exp(-absscore / 10000);
    this.score += relscore;
};

SyncFSFile.prototype.set_state = function (newstate)
{
    this.map_by_state.get(this.state).delete(this);
    this.state = newstate;
    this.map_by_state.get(this.state).add(this);
};

SyncFSFile.prototype.sync = function (rev)
{
    this.set_state("synched");
    this.score = 0;
};

SyncFSFile.prototype.set_old_by_time = function (old_by_time)
{
    this.old_by_time = old_by_time;
};

SyncFSFile.prototype.touch_written = function (event, i)
{
    this.set_state("written");
    this.bump_score(event, i);
};

SyncFSFile.prototype.touch_unknown = function (event, i)
{
    this.set_state("unknown");
};

SyncFSFile.prototype.touch_deleted = function (event, i)
{
    this.set_state("deleted");
    this.bump_score(event, i);
};

SyncFSFile.prototype.touch = function (event, i)
{
    if (event.done) {
	if (event.command === "create" ||
	    event.command === "write" ||
	    event.command === "symlink" ||
	    event.command === "rename" && i == 1)
	    this.touch_written(event, i);
	else if (event.command === "unlink" ||
		 event.command === "rename" && i == 0)
	    this.touch_deleted(event, i);
    } else {
	this.touch_unknown(event, i);
    }
}

async function update_score(event)
{
    let paths = [];
    if (event.path !== undefined)
	paths.push(event.path);
    else {
	paths.push(event.path1);
	paths.push(event.path2);
    }

    for (let i = 0; i < paths.length; i++) {
	let f = SyncFSFile.by_path(paths[i], event);
	f.touch(event, i);
    }
}

async function resolve_starid(event)
{
    let cmdline =
	(await fs.readFile("/proc/" + event.pid + "/cmdline", "utf-8"))
	.split("\0");

    event.cmdline = cmdline;
}

async function main()
{
    let input = oldfs.createReadStream(fifopath + "/fuse-to-daemon");
    let output = oldfs.createWriteStream(fifopath + "/daemon-to-fuse",
					 { highWaterMark: 0 });

    let rl = readline.createInterface({
	input,
    });

    let notify_queues = new Map();

    async function notify(rev)
    {
	for (let remote of remotes) {
	    if (!notify_queues.has(remote))
		notify_queues.set(remote, []);
	    let q = notify_queues.get(remote);
	    q.push(rev);
	    if (q.length === 1) {
		let cb; cb = () => {
		    if (q.length) {
			child_process.spawn("ssh", [remote, "echo", q.shift(), ">>", "sync/syncfs-pings"], {stdio: ["pipe", "inherit", "inherit"]}, cb);
		    }
		};
		cb();
	    }
	}
    }

    let chdired = false;
    async function add_or_del_files(state, args)
    {
	if (!chdired) {
	    process.chdir("c00git");
	    chdired = true;
	}
	let max_files = 512;
	let files = SyncFSFile.by_state(state, max_files);
	if (files.length === 0)
	    return;

	let paths = files.map(f => f.path);
	let stdin = paths.join("\0");
	let error = await new Promise(r => {
	    let child =
		child_process.spawn("git", args, {stdio: ["pipe", "inherit", "inherit"]});
	    child.on("close", code => r(code !== 0));
	    child.stdin.write(stdin);
	    child.stdin.end();
	});
	if (error)
	    return;
	error = await new Promise(r => {
	    let child =
		child_process.spawn("git", ["commit", "--allow-empty", "-m", "automatic commit"], {stdio: ["inherit", "inherit", "inherit"]})
	    child.on("close", code => r(code !== 0));
	});
	if (error)
	    return;
	let rev;
	error = await new Promise(r => {
	    let child =
		child_process.spawn("git", ["rev-parse", "HEAD"],
				    { stdio: ["inherit", "pipe", "inherit"] });
	    rev = "";
	    child.stdout.on("data", data => {
		rev += data.toString();
	    });
	    child.on("close", code => r(code !== 0));
	})
	if (error)
	    return;
	for (let file of files)
	    file.sync(rev);
	while (rev.length && rev[rev.length-1] === "\n")
	    rev = rev.substr(0, rev.length - 1);
	return rev;
    }

    async function add_files()
    {
	return await add_or_del_files("written", ["add", "--ignore-removal", "--pathspec-from-file=-", "--pathspec-file-nul"]);
    }

    async function del_files()
    {
	return await add_or_del_files("deleted", ["rm", "--ignore-unmatch", "--pathspec-from-file=-", "--pathspec-file-nul"]);
    }

    async function handle_event(event)
    {
	await resolve_starid(event);
	output.write("\n");
	await update_score(event);
    }

    let event = {};
    let timer;
    let timerTriggered;
    let timerRunning;

    async function timeout()
    {
	timer = undefined;
	timerTriggered = true;
	if (timerRunning) {
	    return;
	}
	while (timerTriggered) {
	    timerTriggered = false;
	    timerRunning = true;
	    let rev;
	    if ((rev = await add_files()) !== undefined ||
		(rev = await del_files()) !== undefined) {
		notify(rev);
		timerTriggered = true;
	    }
	    timerRunning = false;
	}
    }

    rl.on("line", async function (line) {
	console.log(line);
	if (line === "") {
	    event.done = true;
	    await handle_event(event);
	} else {
	    event = JSON.parse(line);
	    event.done = false;
	    await handle_event(event);
	}
	if (timer !== undefined) {
	    clearTimeout(timer);
	}
	timer = setTimeout(timeout, 5000);
    });

    rl.on("close", function () {
	console.error("should be terminating properly");
    });
}

main();
