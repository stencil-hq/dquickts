# DeltaBlue incremental constraint solver (objects, inheritance, super(),
# classmethods, list-subclass collections). Condensed port of the classic.
import sys
import time


class OrderedCollection(list):
    pass


class Strength:
    REQUIRED = None
    STRONG_PREFERRED = None
    PREFERRED = None
    STRONG_DEFAULT = None
    NORMAL = None
    WEAK_DEFAULT = None
    WEAKEST = None

    def __init__(self, strength, name):
        self.strength = strength
        self.name = name

    @classmethod
    def stronger(cls, s1, s2):
        return s1.strength < s2.strength

    @classmethod
    def weaker(cls, s1, s2):
        return s1.strength > s2.strength

    @classmethod
    def weakest_of(cls, s1, s2):
        return s1 if cls.weaker(s1, s2) else s2

    def next_weaker(self):
        # Mirrors the original benchmark's (quirky) table.
        return {
            0: Strength.WEAKEST,
            1: Strength.WEAK_DEFAULT,
            2: Strength.NORMAL,
            3: Strength.STRONG_DEFAULT,
            4: Strength.PREFERRED,
            5: Strength.REQUIRED,
        }[self.strength]


Strength.REQUIRED = Strength(0, "required")
Strength.STRONG_PREFERRED = Strength(1, "strongPreferred")
Strength.PREFERRED = Strength(2, "preferred")
Strength.STRONG_DEFAULT = Strength(3, "strongDefault")
Strength.NORMAL = Strength(4, "normal")
Strength.WEAK_DEFAULT = Strength(5, "weakDefault")
Strength.WEAKEST = Strength(6, "weakest")


class Direction:
    NONE = 0
    FORWARD = 1
    BACKWARD = -1


planner = None
failures = 0


class Constraint:
    def __init__(self, strength):
        self.strength = strength

    def add_constraint(self):
        self.add_to_graph()
        planner.incremental_add(self)

    def satisfy(self, mark):
        global failures
        self.choose_method(mark)
        if not self.is_satisfied():
            if self.strength is Strength.REQUIRED:
                failures += 1
            return None
        self.mark_inputs(mark)
        out = self.output()
        overridden = out.determined_by
        if overridden is not None:
            overridden.mark_unsatisfied()
        out.determined_by = self
        if not planner.add_propagate(self, mark):
            failures += 1
        out.mark = mark
        return overridden

    def destroy_constraint(self):
        if self.is_satisfied():
            planner.incremental_remove(self)
        else:
            self.remove_from_graph()

    def is_input(self):
        return False


class UnaryConstraint(Constraint):
    def __init__(self, v, strength):
        super().__init__(strength)
        self.my_output = v
        self.satisfied = False
        self.add_constraint()

    def add_to_graph(self):
        self.my_output.add_constraint(self)
        self.satisfied = False

    def choose_method(self, mark):
        self.satisfied = (self.my_output.mark != mark
                          and Strength.stronger(self.strength, self.my_output.walk_strength))

    def is_satisfied(self):
        return self.satisfied

    def mark_inputs(self, mark):
        pass

    def output(self):
        return self.my_output

    def recalculate(self):
        self.my_output.walk_strength = self.strength
        self.my_output.stay = not self.is_input()
        if self.my_output.stay:
            self.execute()

    def mark_unsatisfied(self):
        self.satisfied = False

    def inputs_known(self, mark):
        return True

    def remove_from_graph(self):
        if self.my_output is not None:
            self.my_output.remove_constraint(self)
            self.satisfied = False


class StayConstraint(UnaryConstraint):
    def execute(self):
        pass


class EditConstraint(UnaryConstraint):
    def is_input(self):
        return True

    def execute(self):
        pass


class BinaryConstraint(Constraint):
    def __init__(self, v1, v2, strength):
        super().__init__(strength)
        self.v1 = v1
        self.v2 = v2
        self.direction = Direction.NONE
        self.add_constraint()

    def choose_method(self, mark):
        v1, v2 = self.v1, self.v2
        if v1.mark == mark:
            if v2.mark != mark and Strength.stronger(self.strength, v2.walk_strength):
                self.direction = Direction.FORWARD
            else:
                self.direction = Direction.NONE
            return
        if v2.mark == mark:
            if v1.mark != mark and Strength.stronger(self.strength, v1.walk_strength):
                self.direction = Direction.BACKWARD
            else:
                self.direction = Direction.NONE
            return
        if Strength.weaker(v1.walk_strength, v2.walk_strength):
            if Strength.stronger(self.strength, v1.walk_strength):
                self.direction = Direction.BACKWARD
            else:
                self.direction = Direction.NONE
        else:
            if Strength.stronger(self.strength, v2.walk_strength):
                self.direction = Direction.FORWARD
            else:
                self.direction = Direction.BACKWARD

    def add_to_graph(self):
        self.v1.add_constraint(self)
        self.v2.add_constraint(self)
        self.direction = Direction.NONE

    def is_satisfied(self):
        return self.direction != Direction.NONE

    def mark_inputs(self, mark):
        self.input().mark = mark

    def input(self):
        return self.v1 if self.direction == Direction.FORWARD else self.v2

    def output(self):
        return self.v2 if self.direction == Direction.FORWARD else self.v1

    def recalculate(self):
        ihn = self.input()
        out = self.output()
        out.walk_strength = Strength.weakest_of(self.strength, ihn.walk_strength)
        out.stay = ihn.stay
        if out.stay:
            self.execute()

    def mark_unsatisfied(self):
        self.direction = Direction.NONE

    def inputs_known(self, mark):
        i = self.input()
        return i.mark == mark or i.stay or i.determined_by is None

    def remove_from_graph(self):
        if self.v1 is not None:
            self.v1.remove_constraint(self)
        if self.v2 is not None:
            self.v2.remove_constraint(self)
        self.direction = Direction.NONE


class ScaleConstraint(BinaryConstraint):
    def __init__(self, src, scale, offset, dest, strength):
        self.direction = Direction.NONE
        self.scale = scale
        self.offset = offset
        super().__init__(src, dest, strength)

    def add_to_graph(self):
        super().add_to_graph()
        self.scale.add_constraint(self)
        self.offset.add_constraint(self)

    def remove_from_graph(self):
        super().remove_from_graph()
        if self.scale is not None:
            self.scale.remove_constraint(self)
        if self.offset is not None:
            self.offset.remove_constraint(self)

    def mark_inputs(self, mark):
        super().mark_inputs(mark)
        self.scale.mark = mark
        self.offset.mark = mark

    def execute(self):
        if self.direction == Direction.FORWARD:
            self.v2.value = self.v1.value * self.scale.value + self.offset.value
        else:
            self.v1.value = (self.v2.value - self.offset.value) // self.scale.value

    def recalculate(self):
        ihn = self.input()
        out = self.output()
        out.walk_strength = Strength.weakest_of(self.strength, ihn.walk_strength)
        out.stay = ihn.stay and self.scale.stay and self.offset.stay
        if out.stay:
            self.execute()


class EqualityConstraint(BinaryConstraint):
    def execute(self):
        self.output().value = self.input().value


class Variable:
    def __init__(self, name, initial_value=0):
        self.name = name
        self.value = initial_value
        self.constraints = OrderedCollection()
        self.determined_by = None
        self.mark = 0
        self.walk_strength = Strength.WEAKEST
        self.stay = True

    def add_constraint(self, c):
        self.constraints.append(c)

    def remove_constraint(self, c):
        self.constraints.remove(c)
        if self.determined_by is c:
            self.determined_by = None


class Plan:
    def __init__(self):
        self.v = OrderedCollection()

    def add_constraint(self, c):
        self.v.append(c)

    def __len__(self):
        return len(self.v)

    def execute(self):
        for c in self.v:
            c.execute()


class Planner:
    def __init__(self):
        self.current_mark = 0

    def incremental_add(self, c):
        mark = self.new_mark()
        overridden = c.satisfy(mark)
        while overridden is not None:
            overridden = overridden.satisfy(mark)

    def incremental_remove(self, c):
        out = c.output()
        c.mark_unsatisfied()
        c.remove_from_graph()
        unsatisfied = self.remove_propagate_from(out)
        strength = Strength.REQUIRED
        while True:
            for u in unsatisfied:
                if u.strength is strength:
                    self.incremental_add(u)
            strength = strength.next_weaker()
            if strength is Strength.WEAKEST:
                break

    def new_mark(self):
        self.current_mark += 1
        return self.current_mark

    def make_plan(self, sources):
        mark = self.new_mark()
        plan = Plan()
        todo = sources
        while todo:
            c = todo.pop(0)
            if c.output().mark != mark and c.inputs_known(mark):
                plan.add_constraint(c)
                c.output().mark = mark
                self.add_constraints_consuming_to(c.output(), todo)
        return plan

    def extract_plan_from_constraints(self, constraints):
        sources = OrderedCollection()
        for c in constraints:
            if c.is_input() and c.is_satisfied():
                sources.append(c)
        return self.make_plan(sources)

    def add_propagate(self, c, mark):
        todo = OrderedCollection()
        todo.append(c)
        while todo:
            d = todo.pop(0)
            if d.output().mark == mark:
                self.incremental_remove(c)
                return False
            d.recalculate()
            self.add_constraints_consuming_to(d.output(), todo)
        return True

    def remove_propagate_from(self, out):
        out.determined_by = None
        out.walk_strength = Strength.WEAKEST
        out.stay = True
        unsatisfied = OrderedCollection()
        todo = OrderedCollection()
        todo.append(out)
        while todo:
            v = todo.pop(0)
            for c in v.constraints:
                if not c.is_satisfied():
                    unsatisfied.append(c)
            determining = v.determined_by
            for c in v.constraints:
                if c is not determining and c.is_satisfied():
                    c.recalculate()
                    todo.append(c.output())
        return unsatisfied

    def add_constraints_consuming_to(self, v, coll):
        determining = v.determined_by
        for c in v.constraints:
            if c is not determining and c.is_satisfied():
                coll.append(c)


def chain_test(n):
    global planner, failures
    planner = Planner()
    prev = first = last = None
    for i in range(n + 1):
        v = Variable("v%s" % i)
        if prev is not None:
            EqualityConstraint(prev, v, Strength.REQUIRED)
        if i == 0:
            first = v
        if i == n:
            last = v
        prev = v
    StayConstraint(last, Strength.STRONG_DEFAULT)
    edit = EditConstraint(first, Strength.PREFERRED)
    edits = OrderedCollection()
    edits.append(edit)
    plan = planner.extract_plan_from_constraints(edits)
    total = 0
    for i in range(100):
        first.value = i
        plan.execute()
        if last.value != i:
            failures += 1
        total += last.value
    return total


def change(v, new_value):
    edit = EditConstraint(v, Strength.PREFERRED)
    edits = OrderedCollection()
    edits.append(edit)
    plan = planner.extract_plan_from_constraints(edits)
    for _ in range(10):
        v.value = new_value
        plan.execute()
    edit.destroy_constraint()


def projection_test(n):
    global planner, failures
    planner = Planner()
    scale = Variable("scale", 10)
    offset = Variable("offset", 1000)
    src = dst = None
    dests = OrderedCollection()
    for i in range(n):
        src = Variable("src%s" % i, i)
        dst = Variable("dst%s" % i, i)
        dests.append(dst)
        StayConstraint(src, Strength.NORMAL)
        ScaleConstraint(src, scale, offset, dst, Strength.REQUIRED)
    change(src, 17)
    if dst.value != 1170:
        failures += 1
    change(dst, 1050)
    if src.value != 5:
        failures += 1
    change(scale, 5)
    for i in range(n - 1):
        if dests[i].value != i * 5 + 1000:
            failures += 1
    change(offset, 2000)
    for i in range(n - 1):
        if dests[i].value != i * 5 + 2000:
            failures += 1
    return sum(d.value for d in dests)


def run(iterations):
    total = 0
    for _ in range(iterations):
        total += chain_test(100)
        total += projection_test(100)
    return f"{total} failures={failures} marks={planner.current_mark}"


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 60
    run(2)
    t0 = time.perf_counter()
    result = run(n)
    t1 = time.perf_counter()
    print(f"time {t1 - t0:.6f}")
    print(f"check {result}")


main()
