// ES module imported from Python by test_interop.py (js.import_module).
export const answer = 42;
export function twice(x) {
    return 2 * x;
}
export class Counter {
    constructor(start) {
        this.n = start;
    }
    inc() {
        return ++this.n;
    }
}
