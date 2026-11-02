# Richards OS task scheduler simulation (method calls, attribute access,
# inheritance, linked lists). Condensed port of the classic benchmark.
import sys
import time

I_IDLE = 1
I_WORK = 2
I_HANDLERA = 3
I_HANDLERB = 4
I_DEVA = 5
I_DEVB = 6

K_DEV = 1000
K_WORK = 1001

BUFSIZE = 4
TASKTABSIZE = 10
A = ord("A")


class Packet:
    def __init__(self, link, ident, kind):
        self.link = link
        self.ident = ident
        self.kind = kind
        self.datum = 0
        self.data = [0] * BUFSIZE

    def append_to(self, lst):
        self.link = None
        if lst is None:
            return self
        p = lst
        nxt = p.link
        while nxt is not None:
            p = nxt
            nxt = p.link
        p.link = self
        return lst


class TaskRec:
    pass


class DeviceTaskRec(TaskRec):
    def __init__(self):
        self.pending = None


class IdleTaskRec(TaskRec):
    def __init__(self):
        self.control = 1
        self.count = 10000


class HandlerTaskRec(TaskRec):
    def __init__(self):
        self.work_in = None
        self.device_in = None

    def work_in_add(self, p):
        self.work_in = p.append_to(self.work_in)
        return self.work_in

    def device_in_add(self, p):
        self.device_in = p.append_to(self.device_in)
        return self.device_in


class WorkerTaskRec(TaskRec):
    def __init__(self):
        self.destination = I_HANDLERA
        self.count = 0


class TaskState:
    def __init__(self):
        self.packet_pending = True
        self.task_waiting = False
        self.task_holding = False

    def packet_pending_state(self):
        self.packet_pending = True
        self.task_waiting = False
        self.task_holding = False
        return self

    def waiting(self):
        self.packet_pending = False
        self.task_waiting = True
        self.task_holding = False
        return self

    def running(self):
        self.packet_pending = False
        self.task_waiting = False
        self.task_holding = False
        return self

    def waiting_with_packet(self):
        self.packet_pending = True
        self.task_waiting = True
        self.task_holding = False
        return self

    def is_task_holding_or_waiting(self):
        return self.task_holding or (not self.packet_pending and self.task_waiting)

    def is_waiting_with_packet(self):
        return self.packet_pending and self.task_waiting and not self.task_holding


class TaskWorkArea:
    def __init__(self):
        self.task_tab = [None] * TASKTABSIZE
        self.task_list = None
        self.hold_count = 0
        self.qpkt_count = 0


area = TaskWorkArea()


class Task(TaskState):
    def __init__(self, ident, priority, inp, initial_state, handle):
        self.link = area.task_list
        self.ident = ident
        self.priority = priority
        self.input = inp
        self.packet_pending = initial_state.packet_pending
        self.task_waiting = initial_state.task_waiting
        self.task_holding = initial_state.task_holding
        self.handle = handle
        area.task_list = self
        area.task_tab[ident] = self

    def fn(self, pkt, r):
        raise NotImplementedError

    def add_packet(self, p, old):
        if self.input is None:
            self.input = p
            self.packet_pending = True
            if self.priority > old.priority:
                return self
        else:
            p.append_to(self.input)
        return old

    def run_task(self):
        if self.is_waiting_with_packet():
            msg = self.input
            self.input = msg.link
            if self.input is None:
                self.running()
            else:
                self.packet_pending_state()
        else:
            msg = None
        return self.fn(msg, self.handle)

    def wait_task(self):
        self.task_waiting = True
        return self

    def hold(self):
        area.hold_count += 1
        self.task_holding = True
        return self.link

    def release(self, i):
        t = self.findtcb(i)
        t.task_holding = False
        if t.priority > self.priority:
            return t
        return self

    def qpkt(self, pkt):
        t = self.findtcb(pkt.ident)
        area.qpkt_count += 1
        pkt.link = None
        pkt.ident = self.ident
        return t.add_packet(pkt, self)

    def findtcb(self, ident):
        t = area.task_tab[ident]
        if t is None:
            raise Exception("Bad task id %d" % ident)
        return t


class DeviceTask(Task):
    def fn(self, pkt, r):
        if pkt is None:
            pkt = r.pending
            if pkt is None:
                return self.wait_task()
            r.pending = None
            return self.qpkt(pkt)
        r.pending = pkt
        return self.hold()


class HandlerTask(Task):
    def fn(self, pkt, r):
        if pkt is not None:
            if pkt.kind == K_WORK:
                r.work_in_add(pkt)
            else:
                r.device_in_add(pkt)
        work = r.work_in
        if work is None:
            return self.wait_task()
        count = work.datum
        if count >= BUFSIZE:
            r.work_in = work.link
            return self.qpkt(work)
        dev = r.device_in
        if dev is None:
            return self.wait_task()
        r.device_in = dev.link
        dev.datum = work.data[count]
        work.datum = count + 1
        return self.qpkt(dev)


class IdleTask(Task):
    def fn(self, pkt, r):
        r.count -= 1
        if r.count == 0:
            return self.hold()
        if r.control & 1 == 0:
            r.control //= 2
            return self.release(I_DEVA)
        r.control = r.control // 2 ^ 0xD008
        return self.release(I_DEVB)


class WorkTask(Task):
    def fn(self, pkt, r):
        if pkt is None:
            return self.wait_task()
        dest = I_HANDLERB if r.destination == I_HANDLERA else I_HANDLERA
        r.destination = dest
        pkt.ident = dest
        pkt.datum = 0
        for i in range(BUFSIZE):
            r.count += 1
            if r.count > 26:
                r.count = 1
            pkt.data[i] = A + r.count - 1
        return self.qpkt(pkt)


def schedule():
    t = area.task_list
    while t is not None:
        if t.is_task_holding_or_waiting():
            t = t.link
        else:
            t = t.run_task()


def run(iterations):
    total_hold = total_qpkt = 0
    for _ in range(iterations):
        area.hold_count = 0
        area.qpkt_count = 0
        area.task_list = None
        IdleTask(I_IDLE, 1, 10000, TaskState().running(), IdleTaskRec())
        wkq = Packet(None, 0, K_WORK)
        wkq = Packet(wkq, 0, K_WORK)
        WorkTask(I_WORK, 1000, wkq, TaskState().waiting_with_packet(), WorkerTaskRec())
        wkq = Packet(None, I_DEVA, K_DEV)
        wkq = Packet(wkq, I_DEVA, K_DEV)
        wkq = Packet(wkq, I_DEVA, K_DEV)
        HandlerTask(I_HANDLERA, 2000, wkq, TaskState().waiting_with_packet(), HandlerTaskRec())
        wkq = Packet(None, I_DEVB, K_DEV)
        wkq = Packet(wkq, I_DEVB, K_DEV)
        wkq = Packet(wkq, I_DEVB, K_DEV)
        HandlerTask(I_HANDLERB, 3000, wkq, TaskState().waiting_with_packet(), HandlerTaskRec())
        DeviceTask(I_DEVA, 4000, None, TaskState().waiting(), DeviceTaskRec())
        DeviceTask(I_DEVB, 5000, None, TaskState().waiting(), DeviceTaskRec())
        schedule()
        total_hold += area.hold_count
        total_qpkt += area.qpkt_count
    return f"{total_hold} {total_qpkt}"


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 4
    run(1)
    t0 = time.perf_counter()
    result = run(n)
    t1 = time.perf_counter()
    print(f"time {t1 - t0:.6f}")
    print(f"check {result}")


main()
