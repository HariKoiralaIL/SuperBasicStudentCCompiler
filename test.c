int add(int a, int b) {
    return a + b;
}

int fib(int n) {
    if (n < 2) {
        return n;
    }
    return fib(n - 1) + fib(n - 2);
}

int main() {
    int x = 0;
    int i = 0;
    for (i = 0; i < 10; i = i + 1) {
        x = x + fib(i);
    }
    int *p = &x;
    *p = *p + add(1, 2);
    return x;
}