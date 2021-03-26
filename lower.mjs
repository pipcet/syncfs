import * as oldfs from "fs";
import * as fs from "fs/promises";
import * as readline from "readline";
import * as child_process from "child_process";
import { setTimeout, clearTimeout } from "timers";

let tmpdir = "/home/pip";
let tmpcounter = Math.floor(Date.now());
let tmpnam = () => {
    return tmpdir + "/detach-token-" + tmpcounter++;
};
let inherit = {stdio:["inherit","inherit","inherit"]};

let myid = "myid";

async function copy_recursively(src, dst, files)
{
    for (let file of files) {
	try {
	    let dir = await fs.opendir(src + "/" + file);
	    await fs.mkdir(dst + "/" + file, { recursive: true });
	    for await (let dirent of dir) {
		await copy_recursively(src + "/" + file,
				       dst + "/" + file,
				       [dirent.name]);
	    }
	} catch (e) {
	    if (e.code === "ENOENT")
		continue;
	    if (e.code !== "ENOTDIR")
		throw e;
	    await fs.copyFile(src + "/" + file,
			      dst + "/" + file,
			      oldfs.constants.COPYFILE_FICLONE);
	}
    }
}

async function with_suspended_fs(mount, c00git, f)
{
    let n1 = tmpnam();
    let n2 = tmpnam();
    let c1 = child_process.spawn("setfattr", ["-n", "syncfs.detach-for-lowering", "-v", n1, "mount/"], {stdio:["inherit", "inherit", "inherit"]});

    let c2 = child_process.spawn("setfattr", ["-n", "syncfs.detach-for-lowering", "-v", n2, "c00git/"], {stdio:["inherit", "inherit", "inherit"]});

    try {
	await new Promise(r => setTimeout(r, 1000));
	while (await new Promise(r =>
	    child_process.spawn("sudo", ["umount", "merge"],
				inherit).on("close", r)) !== 0) {
	    await new Promise(r => setTimeout(r, 1000));
	}
	try {
	    await f();
	} finally {
	    await new Promise(r =>
		child_process.spawn("sudo", ["mount", "-toverlay", "-olowerdir=data/lower,upperdir=data/upper,workdir=data/work", "overlay", "merge"], inherit).on("close", r));
	}
    } finally {
	await fs.unlink(n1);
	await fs.unlink(n2);
    }
}

async function lower_copy(files)
{
    for (let file of files) {
	let sha512;
	await with_suspended_fs("mount", "c00git", async function () {
	    file = file.replace(/^mount\//, "");
	    await copy_recursively("data/upper", "data/lower", [file]);
	    sha512 = child_process.execSync(`find data/lower/${file} -type f|xargs sha512sum|sort`);
	});
	await fs.mkdir("mount/lowering/" + file + `/${myid}`, {recursive:true});
	let outpath = "mount/lowering/" + file + `/${myid}/sha512`;
	await oldfs.writeFileSync(outpath, sha512);
	console.error(`wrote ${outpath}`)
    }
}

async function lower_remote(remote, files)
{
    let code =
	await new Promise(r =>
	    child_process.spawn("ssh", [remote, "node", "/home/pip/syncfs/lower.mjs", "stage1", remote, "localhost", ...files], {stdio:["inherit", "inherit", "inherit"]}).on("close", r));
    if (code !== 0)
	throw new Error();
}

async function run()
{
    process.chdir("/home/pip/sync"); // XXX, obviously
    let args = process.argv.slice(2);
    let command = args.shift();
    if (command === "stage1") {
	let [local, remote, ...files] = args;
	myid = local;
	console.error("lowering locally");
	await lower_copy(files);
	if (remote !== "" && remote !== "localhost") {
	    console.error("lowering on " + remote);
	    await lower_remote(remote, files);
	}
    } else if (command === "stage2") {
	let [local, remote, ...files] = args;
	myid = local;
	for (let file of files) {
	    file = file.replace(/^mount\//, "");
	    let shasum_local = oldfs.readFileSync("data/upper/lowering/" +
						  file + "/" + myid + "/sha512", "utf-8");
	    let shasum_remote = oldfs.readFileSync("data/upper/lowering/" +
						   file + "/" + process.argv[4] +
						   "/sha512", "utf-8");
	    if (shasum_local !== shasum_remote)
		;//throw "die horribly";
	    await with_suspended_fs("mount", "c00git", async function () {
		await new Promise(r => {
		    child_process.spawn("rm", ["-rf", "--", "data/upper/lowering/" + file],
					inherit)
			.on("close", r);
		});
		await new Promise(r => {
		    child_process.spawn("rm", ["-rf", "--", "data/upper/" + file],
					inherit)
			.on("close", r);
		});
		child_process.execSync(`mkdir -p -- data/upper/.wowo/${file}`);
	    });
	    process.chdir("c00git");
	    await new Promise(r => {
		child_process.spawn("git", ["add", "."],
				    inherit)
		    .on("close", r);
	    });
	    await new Promise(r => {
		child_process.spawn("git", ["commit", "-m", "finalizing"],
				    inherit)
		    .on("close", r);
	    });
	    child_process.execSync("date > ../mount/syncme");
	}
    } else if (command === "stage3") {
	let [local, remote, ...files] = args;
	for (let file of files) {
	    await with_suspended_fs("mount", "c00git", async function () {
		await new Promise(r => {
		    child_process.spawn("rm", ["-rf", "--", "data/upper/lowering/" + file],
					inherit)
			.on("close", r);
		});
		await new Promise(r => {
		    child_process.spawn("rm", ["-rf", "--", "data/upper/" + file],
					inherit)
			.on("close", r);
		});
	    });
	}
    } else if (command === "stage5") {
	let [local, remote, ...files] = args;
	process.chdir("c00git");
	await new Promise(r => {
	    child_process.spawn("git", ["reflog", "expire", "--expire=all", "--all"],
				inherit)
		.on("close", r);
	});
	await new Promise(r => {
	    child_process.spawn("git", ["gc", "--aggressive", "--prune=now"],
				inherit)
		.on("close", r);
	});
	await new Promise(r => {
	    child_process.spawn("git", ["repack", "-ad"],
				inherit)
		.on("close", r);
	});
    }
}

run();
