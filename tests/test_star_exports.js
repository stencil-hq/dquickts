import { assert } from "./assert.js";
import * as ns from "./fixture_star_root.js";
import { w, v, y, z } from "./fixture_star_root.js";

/* 'x' is exported by two star exports: ambiguous, not in the namespace.
   'w' is reached by two paths to the same binding. The star exports of
   fixture_star_c.js are circular. 'default' is not exported by a star
   export. */
assert(Object.keys(ns).join(), "ns,own,v,w,y,y2,z");
assert(ns.w + ns.v + ns.y + ns.y2 + ns.z + ns.own, "ddadcown");
assert(ns.ns.w, "d");
assert(w + v + y + z, "ddac");

/* importing the ambiguous name fails */
var e = await import("./fixture_star_import_x.js").then(() => null, e => e);
assert(e instanceof SyntaxError, true);
assert(e.message.includes("ambiguous"), true);
