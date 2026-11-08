# Small ray tracer (classes, operator overloading, float math, recursion).
import sys
import time


class Vec:
    def __init__(self, x, y, z):
        self.x = x
        self.y = y
        self.z = z

    def __add__(self, o):
        return Vec(self.x + o.x, self.y + o.y, self.z + o.z)

    def __sub__(self, o):
        return Vec(self.x - o.x, self.y - o.y, self.z - o.z)

    def __mul__(self, s):
        return Vec(self.x * s, self.y * s, self.z * s)

    def dot(self, o):
        return self.x * o.x + self.y * o.y + self.z * o.z

    def mul(self, o):
        return Vec(self.x * o.x, self.y * o.y, self.z * o.z)

    def length(self):
        return self.dot(self) ** 0.5

    def normalized(self):
        return self * (1.0 / self.length())


class Ray:
    def __init__(self, origin, direction):
        self.origin = origin
        self.direction = direction.normalized()

    def point_at(self, t):
        return self.origin + self.direction * t


class Material:
    def __init__(self, color, diffuse, specular, reflect):
        self.color = color
        self.diffuse = diffuse
        self.specular = specular
        self.reflect = reflect


class Shape:
    def __init__(self, material):
        self.material = material

    def color_at(self, point):
        return self.material.color


class Sphere(Shape):
    def __init__(self, center, radius, material):
        super().__init__(material)
        self.center = center
        self.radius = radius

    def intersect(self, ray):
        oc = ray.origin - self.center
        b = oc.dot(ray.direction)
        c = oc.dot(oc) - self.radius * self.radius
        disc = b * b - c
        if disc < 0.0:
            return None
        sq = disc ** 0.5
        t = -b - sq
        if t > 1e-6:
            return t
        t = -b + sq
        if t > 1e-6:
            return t
        return None

    def normal_at(self, p):
        return (p - self.center).normalized()


class Plane(Shape):
    def __init__(self, point, normal, material):
        super().__init__(material)
        self.point = point
        self.normal = normal.normalized()

    def intersect(self, ray):
        denom = ray.direction.dot(self.normal)
        if -1e-9 < denom < 1e-9:
            return None
        t = (self.point - ray.origin).dot(self.normal) / denom
        return t if t > 1e-6 else None

    def normal_at(self, p):
        return self.normal

    def color_at(self, p):
        # Checkerboard.
        if (int(p.x + 1000.0) + int(p.z + 1000.0)) % 2 == 0:
            return self.material.color
        return Vec(0.1, 0.1, 0.1)


class Scene:
    def __init__(self):
        self.objects = []
        self.lights = []
        self.ambient = 0.1

    def nearest(self, ray):
        best_t = None
        best = None
        for obj in self.objects:
            t = obj.intersect(ray)
            if t is not None and (best_t is None or t < best_t):
                best_t = t
                best = obj
        return best, best_t

    def shadowed(self, point, light_pos):
        to_light = light_pos - point
        dist = to_light.length()
        ray = Ray(point, to_light)
        for obj in self.objects:
            t = obj.intersect(ray)
            if t is not None and t < dist:
                return True
        return False

    def trace(self, ray, depth):
        obj, t = self.nearest(ray)
        if obj is None:
            return Vec(0.2, 0.3, 0.5)
        p = ray.point_at(t)
        n = obj.normal_at(p)
        m = obj.material
        base = obj.color_at(p)
        color = base * self.ambient
        for light_pos, light_color in self.lights:
            if self.shadowed(p, light_pos):
                continue
            l = (light_pos - p).normalized()
            lam = n.dot(l)
            if lam > 0.0:
                color = color + base.mul(light_color) * (lam * m.diffuse)
                h = (l - ray.direction).normalized()
                spec = n.dot(h)
                if spec > 0.0:
                    color = color + light_color * (m.specular * spec ** 32)
        if depth > 0 and m.reflect > 0.0:
            d = ray.direction
            r = d - n * (2.0 * d.dot(n))
            color = color + self.trace(Ray(p, r), depth - 1) * m.reflect
        return color


def build_scene():
    s = Scene()
    s.objects.append(Plane(Vec(0.0, -1.0, 0.0), Vec(0.0, 1.0, 0.0),
                           Material(Vec(0.9, 0.9, 0.9), 0.8, 0.0, 0.2)))
    s.objects.append(Sphere(Vec(0.0, 0.0, 5.0), 1.0, Material(Vec(1.0, 0.2, 0.2), 0.7, 0.5, 0.3)))
    s.objects.append(Sphere(Vec(-2.0, 0.2, 6.0), 1.2, Material(Vec(0.2, 1.0, 0.2), 0.7, 0.3, 0.2)))
    s.objects.append(Sphere(Vec(2.0, -0.3, 4.5), 0.7, Material(Vec(0.2, 0.2, 1.0), 0.6, 0.8, 0.4)))
    s.lights.append((Vec(-5.0, 5.0, 0.0), Vec(1.0, 1.0, 1.0)))
    s.lights.append((Vec(5.0, 3.0, 2.0), Vec(0.5, 0.4, 0.3)))
    return s


def render(size):
    scene = build_scene()
    eye = Vec(0.0, 0.5, 0.0)
    checksum = 0
    for y in range(size):
        for x in range(size):
            dx = (x - size / 2) / size
            dy = (size / 2 - y) / size
            c = scene.trace(Ray(eye, Vec(dx, dy, 1.0)), 3)
            r = min(255, int(c.x * 255.0))
            g = min(255, int(c.y * 255.0))
            b = min(255, int(c.z * 255.0))
            checksum = (checksum * 31 + (r << 16 | g << 8 | b)) % 1000000007
    return checksum


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 100
    render(10)
    t0 = time.perf_counter()
    result = render(n)
    t1 = time.perf_counter()
    print(f"time {t1 - t0:.6f}")
    print(f"check {result}")


main()
