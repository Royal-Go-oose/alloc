// stack_allocator.cpp — LIFO (стековый) аллокатор, C++11 и новее
//
// Идея: вся память — один непрерывный буфер и «вершина» top_.
//   alloc   — сдвигает вершину вверх (O(1), без поиска свободных блоков);
//   dealloc — разрешено ТОЛЬКО для последнего выделенного блока (LIFO),
//             сдвигает вершину обратно (O(1));
//   marker/rewind — одним действием освобождает всё, что выделено после маркера.
//
// Перед каждым блоком лежит заголовок {prev_top, end}:
//   prev_top — вершина до выделения (куда откатываться при free),
//   end      — вершина после выделения (по ней проверяем, что блок действительно
//              последний, т.е. что порядок освобождения именно LIFO).
//
// Сборка: g++ -std=c++17 -O2 -Wall -Wextra stack_allocator.cpp -o stack_allocator && ./stack_allocator

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <random>
#include <utility>
#include <vector>

class StackAllocator {
public:
    static constexpr size_t DEFAULT_ALIGN = 16;

    struct Marker { size_t top; };

    // Аллокатор владеет собственным буфером.
    // Работает в C++11/14/17: выравнивание буфера делаем вручную,
    // поэтому std::align_val_t (только C++17) не нужен.
    explicit StackAllocator(size_t capacity) : cap_(capacity) {
        raw_ = static_cast<unsigned char*>(::operator new(capacity + DEFAULT_ALIGN));
        base_ = reinterpret_cast<unsigned char*>(
            align_up(reinterpret_cast<uintptr_t>(raw_), DEFAULT_ALIGN));
    }

    // Аллокатор работает поверх чужого буфера (стек, статический массив и т.п.).
    StackAllocator(void* buffer, size_t capacity)
        : base_(static_cast<unsigned char*>(buffer)), cap_(capacity) {}

    ~StackAllocator() {
        ::operator delete(raw_);  // raw_ == nullptr для чужого буфера — delete(nullptr) безопасен
    }

    StackAllocator(const StackAllocator&) = delete;
    StackAllocator& operator=(const StackAllocator&) = delete;

    // Выделить n байт. Возвращает nullptr, если места нет или align не степень двойки.
    void* alloc(size_t n, size_t align = DEFAULT_ALIGN) noexcept {
        if (align == 0 || (align & (align - 1)) != 0) return nullptr;
        if (align < alignof(Header)) align = alignof(Header);
        if (align > cap_) return nullptr;

        uintptr_t base = reinterpret_cast<uintptr_t>(base_);
        uintptr_t p = align_up(base + top_ + sizeof(Header), align);
        size_t payload_off = static_cast<size_t>(p - base);
        if (payload_off > cap_ || n > cap_ - payload_off) return nullptr;

        Header* h = reinterpret_cast<Header*>(p - sizeof(Header));
        h->prev_top = top_;
        h->end = payload_off + n;
        top_ = h->end;
        if (top_ > peak_) peak_ = top_;
        return reinterpret_cast<void*>(p);
    }

    // Освободить блок. Успешно только если p — последний выделенный блок.
    // При нарушении LIFO ничего не меняет и возвращает false.
    bool dealloc(void* p) noexcept {
        if (!p) return true;
        if (!owns_ptr(p)) return false;
        Header* h = header_of(p);
        if (h->end != top_) return false;  // это не вершина стека
        top_ = h->prev_top;
        return true;
    }

    // Изменить размер ПОСЛЕДНЕГО блока на месте (рост или сжатие).
    bool try_resize_top(void* p, size_t new_size) noexcept {
        if (!p || !owns_ptr(p)) return false;
        Header* h = header_of(p);
        if (h->end != top_) return false;
        size_t payload_off = static_cast<size_t>(static_cast<unsigned char*>(p) - base_);
        if (new_size > cap_ - payload_off) return false;
        h->end = payload_off + new_size;
        top_ = h->end;
        if (top_ > peak_) peak_ = top_;
        return true;
    }

    // ----- маркеры: освобождение «пачкой» -----
    Marker marker() const noexcept { return Marker{top_}; }
    void rewind(Marker m) noexcept { if (m.top <= top_) top_ = m.top; }
    void reset() noexcept { top_ = 0; }

    // ----- типизированные обёртки (вызывают конструкторы/деструкторы) -----
    template <class T, class... Args>
    T* make(Args&&... args) {
        void* mem = alloc(sizeof(T), alignof(T));
        if (!mem) return nullptr;
        return ::new (mem) T(std::forward<Args>(args)...);
    }

    template <class T>
    bool destroy(T* p) {
        if (!p) return true;
        if (!owns_ptr(p) || header_of(p)->end != top_) return false;  // проверка ДО деструктора
        p->~T();
        return dealloc(p);
    }

    // ----- статистика -----
    size_t used() const noexcept     { return top_; }
    size_t peak() const noexcept     { return peak_; }
    size_t capacity() const noexcept { return cap_; }

private:
    struct Header { size_t prev_top; size_t end; };

    static uintptr_t align_up(uintptr_t x, size_t a) { return (x + a - 1) & ~(uintptr_t)(a - 1); }

    static Header* header_of(void* p) {
        return reinterpret_cast<Header*>(static_cast<unsigned char*>(p) - sizeof(Header));
    }

    bool owns_ptr(const void* p) const noexcept {
        auto a = reinterpret_cast<uintptr_t>(p);
        auto b = reinterpret_cast<uintptr_t>(base_);
        return a >= b + sizeof(Header) && a <= b + top_;
    }

    unsigned char* raw_ = nullptr;   // то, что вернул operator new (nullptr для чужого буфера)
    unsigned char* base_ = nullptr;  // выровненное начало арены
    size_t cap_;
    size_t top_ = 0;
    size_t peak_ = 0;
};

// RAII: при выходе из области видимости откатывает аллокатор к сохранённому маркеру.
class ScopedRewind {
public:
    explicit ScopedRewind(StackAllocator& a) : a_(a), m_(a.marker()) {}
    ~ScopedRewind() { a_.rewind(m_); }
    ScopedRewind(const ScopedRewind&) = delete;
    ScopedRewind& operator=(const ScopedRewind&) = delete;
private:
    StackAllocator& a_;
    StackAllocator::Marker m_;
};

// ======================= тесты =======================
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); std::exit(1); } } while (0)

struct Tracked {
    static int alive;
    int v;
    explicit Tracked(int x) : v(x) { ++alive; }
    ~Tracked() { --alive; }
};
int Tracked::alive = 0;

int main() {
    // 1. Базовый LIFO
    {
        StackAllocator a(1024);
        void* p1 = a.alloc(100);
        void* p2 = a.alloc(200);
        void* p3 = a.alloc(50);
        CHECK(p1 && p2 && p3);
        CHECK(!a.dealloc(p1));          // нарушение LIFO — отказ
        CHECK(!a.dealloc(p2));
        size_t used_before = a.used();
        CHECK(a.dealloc(p3));
        CHECK(a.used() < used_before);
        CHECK(a.dealloc(p2));
        CHECK(a.dealloc(p1));
        CHECK(a.used() == 0);
        CHECK(a.peak() >= 350);
    }

    // 2. Выравнивание
    {
        StackAllocator a(4096);
        for (size_t al : {8u, 16u, 32u, 64u, 256u}) {
            a.alloc(1, 1);              // «ломаем» текущее смещение
            void* p = a.alloc(10, al);
            CHECK(p && reinterpret_cast<uintptr_t>(p) % al == 0);
        }
        CHECK(a.alloc(1, 3) == nullptr);  // не степень двойки
    }

    // 3. Нехватка памяти и повторное использование после free
    {
        StackAllocator a(256);
        void* p = a.alloc(200);
        CHECK(p);
        CHECK(a.alloc(200) == nullptr);
        CHECK(a.alloc(SIZE_MAX) == nullptr);
        CHECK(a.dealloc(p));
        CHECK(a.alloc(200) == p);       // тот же адрес — память переиспользована
    }

    // 4. Маркеры и ScopedRewind
    {
        StackAllocator a(4096);
        a.alloc(64);
        size_t base_used = a.used();
        {
            ScopedRewind guard(a);
            for (int i = 0; i < 10; ++i) CHECK(a.alloc(100));
            CHECK(a.used() > base_used);
        }
        CHECK(a.used() == base_used);

        auto m = a.marker();
        a.alloc(10); a.alloc(20);
        a.rewind(m);
        CHECK(a.used() == base_used);
    }

    // 5. Изменение размера верхнего блока
    {
        StackAllocator a(1024);
        char* p = static_cast<char*>(a.alloc(10));
        std::memset(p, 'x', 10);
        CHECK(a.try_resize_top(p, 500));
        for (int i = 0; i < 10; ++i) CHECK(p[i] == 'x');
        void* q = a.alloc(8);
        CHECK(!a.try_resize_top(p, 20));  // p уже не вершина
        CHECK(a.dealloc(q));
        CHECK(a.try_resize_top(p, 20));
        CHECK(!a.try_resize_top(p, 5000));
        CHECK(a.dealloc(p));
    }

    // 6. make/destroy вызывают конструкторы и деструкторы
    {
        StackAllocator a(1024);
        Tracked* t1 = a.make<Tracked>(1);
        Tracked* t2 = a.make<Tracked>(2);
        CHECK(Tracked::alive == 2 && t1->v == 1 && t2->v == 2);
        CHECK(!a.destroy(t1));            // не LIFO: деструктор НЕ вызывается
        CHECK(Tracked::alive == 2);
        CHECK(a.destroy(t2));
        CHECK(a.destroy(t1));
        CHECK(Tracked::alive == 0 && a.used() == 0);
    }

    // 7. Работа поверх внешнего буфера
    {
        alignas(16) unsigned char buf[512];
        StackAllocator a(buf, sizeof buf);
        void* p = a.alloc(100);
        CHECK(p >= buf && p < buf + sizeof buf);
        CHECK(a.dealloc(p));
    }

    // 8. Случайный стресс: push/pop со стеком блоков и проверкой данных
    {
        StackAllocator a(1 << 20);
        struct Item { unsigned char* p; size_t n; unsigned char tag; size_t used_before; };
        std::vector<Item> st;
        std::mt19937 rng(2024);
        for (int i = 0; i < 500000; ++i) {
            bool push = st.empty() || (rng() % 100 < 55);
            if (push) {
                size_t n = rng() % 2000;
                size_t al = size_t{1} << (rng() % 7);   // 1..64
                size_t before = a.used();
                auto* p = static_cast<unsigned char*>(a.alloc(n, al));
                if (!p) continue;                        // кончилось место
                CHECK(reinterpret_cast<uintptr_t>(p) % al == 0);
                unsigned char tag = static_cast<unsigned char>(rng());
                std::memset(p, tag, n);
                st.push_back({p, n, tag, before});
            } else {
                Item it = st.back(); st.pop_back();
                for (size_t k = 0; k < it.n; ++k) CHECK(it.p[k] == it.tag);
                CHECK(a.dealloc(it.p));
                CHECK(a.used() == it.used_before);       // вершина вернулась точно
            }
        }
        while (!st.empty()) {
            Item it = st.back(); st.pop_back();
            for (size_t k = 0; k < it.n; ++k) CHECK(it.p[k] == it.tag);
            CHECK(a.dealloc(it.p));
        }
        CHECK(a.used() == 0);
    }

    std::puts("Все тесты пройдены");
    return 0;
}
