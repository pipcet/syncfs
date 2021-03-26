import * as oldfs from "fs";
import * as child_process from "child_process";
import * as fs from "fs/promises";

let config = JSON.parse(oldfs.readFileSync("config.json", "utf-8"));
let whoami = child_process.execSync("hostname").toString();
while (whoami[whoami.length-1] === "\n")
    whoami = whoami.substr(0, whoami.length-1);
if (!config.hosts[whoami])
    throw `lost my name: ${whoami}`;

let remotes = {};
for (let host in config.hosts) {
    if (host === whoami)
	continue;
    remotes[host] = config.hosts[host];
}

export { config, whoami, remotes };

