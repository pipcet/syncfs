import * as oldfs from "fs";
import * as fs from "fs/promises";
import * as readline from "readline";
import * as child_process from "child_process";
import { setTimeout, clearTimeout } from "timers";

let [fifopath, ...remotes] = process.argv.slice(2);

function SyncFSFile(path)
{
    this.path = path;
    this.time = Date.now();
    this.old_by_time = this.time;
    this.set_state("unknown");
}

SyncFSFile.map_by_path = new Map();
SyncFSFile.by_path = function (path) {
    if (!SyncFSFile.map_by_path.has(path))
	SyncFSFile.map_by_path.set(path, new SyncFSFile(path));
    return SyncFSFile.map_by_path.get(path);
};

SyncFSFile.by_state = new Map();
SyncFSFile.by_state.set(undefined, new Set());
SyncFSFile.by_state.set("written", new Set());
SyncFSFile.by_state.set("deleted", new Set());
SyncFSFile.by_state.set("unknown", new Set());
SyncFSFile.by_state.set("synched", new Set());

SyncFSFile.prototype.by_state = SyncFSFile.by_state;

SyncFSFile.prototype.set_state = function (newstate)
{
    this.by_state.get(this.state).delete(this);
    this.state = newstate;
    this.by_state.get(this.state).add(this);
};

SyncFSFile.prototype.sync = function (rev)
{
    this.set_state("synched");
};

SyncFSFile.prototype.set_old_by_time = function (old_by_time)
{
    this.old_by_time = old_by_time;
};

SyncFSFile.prototype.touch_written = function (event, i)
{
    this.set_state("written");
};

SyncFSFile.prototype.touch_unknown = function (event, i)
{
    this.set_state("unknown");
};

SyncFSFile.prototype.touch_deleted = function (event, i)
{
    this.set_state("deleted");
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
	let f = SyncFSFile.by_path(paths[i]);
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

    async function notify(rev)
    {
	for (let remote of remotes) {
	    child_process.spawn("ssh", [remote, "echo", rev, ">>", "sync/syncfs-pings"], {stdio: ["pipe", "inherit", "inherit"]});
	}
    }

    let chdired = false;
    async function add_or_del_files(state, args)
    {
	if (!chdired) {
	    process.chdir("c00git");
	    chdired = true;
	}
	if (SyncFSFile.by_state.get(state).size === 0) {
	    console.error("no files to add");
	    return;
	}

	let files = [];
	let i = 0;
	let max_files = 512;
	for (let file of SyncFSFile.by_state.get(state)) {
	    files.push(file);
	    if (i++ == max_files)
		break;
	}

	let paths = files.map(f => f.path);
	let stdin = paths.join("\0");
	console.log(`adding ${i} files: ${stdin}`)
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
		child_process.spawn("git", ["commit", "--allow-empty", "-m", "automatic commit"], {stdio: ["pipe", "inherit", "inherit"]})
	    child.on("close", code => r(code !== 0));
	});
	if (error)
	    return;
	try {
	    let rev = child_process.execSync("git rev-parse HEAD");
	    for (let file of files)
		file.sync(rev);
	    return rev;
	} catch (err) {
	    console.error(err.stdout.toString());
	}
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
	    console.log(`adding files`);
	    let rev;
	    if ((rev = await add_files()) !== "" ||
		(rev = await del_files()) !== "") {
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
