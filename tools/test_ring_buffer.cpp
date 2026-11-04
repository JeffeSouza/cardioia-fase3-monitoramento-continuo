// Testes da fila circular, executados no computador (sem ESP32).
#include <cassert>
#include <cstdio>
#include <initializer_list>
#include "../firmware/ring_buffer.h"

int main() {
  RingBuffer<int> b;
  int v = 0;

  // Sem begin(), a fila nao aceita dados, mas tambem nao falha
  assert(!b.push(1) && b.dropped() == 1 && b.capacity() == 0);

  RingBuffer<int> a;
  assert(a.begin(3) && a.capacity() == 3);

  // Fila vazia
  assert(a.empty() && !a.peek(v) && !a.pop());

  // Ordem FIFO
  assert(a.push(1) && a.push(2));
  assert(a.peek(v) && v == 1 && a.size() == 2);

  // peek nao remove: falha de envio nao perde a amostra
  assert(a.peek(v) && v == 1 && a.size() == 2);
  assert(a.pop() && a.peek(v) && v == 2);

  // Fila cheia descarta a mais antiga e contabiliza
  RingBuffer<int> c;
  c.begin(3);
  c.push(1); c.push(2); c.push(3);
  assert(c.dropped() == 0);
  assert(!c.push(4));
  assert(c.dropped() == 1 && c.size() == 3);

  // A ordem sobrevive a volta completa do indice
  for (int expected : {2, 3, 4}) {
    assert(c.peek(v) && v == expected);
    c.pop();
  }
  assert(c.empty());

  // Uso prolongado com capacidade cheia
  RingBuffer<int> d;
  d.begin(5);
  for (int i = 1; i <= 10000; i++) d.push(i);
  assert(d.size() == 5 && d.dropped() == 9995);
  assert(d.peek(v) && v == 9996);

  puts("OK: todos os testes da fila circular passaram");
  return 0;
}
