#!/usr/bin/env python3
"""Implementacion de referencia del checksum del payload (D1, D22).

Escrita desde la especificacion de docs/decisiones_diseno.md y NO a partir
de src/payload.c: es con esto que se calcularon los valores congelados en
tests/test_payload.c, para que la prueba compare el C contra algo
independiente en vez de contra si mismo.

    python3 scripts/fnv_ref.py            # los vectores de la prueba
    python3 scripts/fnv_ref.py 7 42       # un producer_id y sequence sueltos

El orden de bytes es el del host (D1 no normaliza endianness: la ejecucion
es de un solo host). En una maquina big-endian los valores serian otros.
"""
import sys

BASIS, PRIME, MASK32 = 0x811C9DC5, 0x01000193, 0xFFFFFFFF
FILL_SIZE = 16


def fill(producer_id, sequence):
    """fill[i] = (producer_id + sequence + i) % 256"""
    return bytes((producer_id + sequence + i) % 256 for i in range(FILL_SIZE))


def checksum(producer_id, sequence):
    """FNV-1a sobre sequence (8) + producer_id (4) + fill (16) = 28 bytes."""
    data = (sequence.to_bytes(8, sys.byteorder)
            + producer_id.to_bytes(4, sys.byteorder)
            + fill(producer_id, sequence))
    assert len(data) == 28, len(data)
    h = BASIS
    for byte in data:
        h = ((h ^ byte) * PRIME) & MASK32
    return h


VECTORS = [(1, 1), (0, 0), (1, 2), (2, 1), (7, 42),
           (0xFFFFFFFF, 0xFFFFFFFFFFFFFFFF)]

if __name__ == "__main__":
    pairs = VECTORS
    if len(sys.argv) == 3:
        pairs = [(int(sys.argv[1]), int(sys.argv[2]))]
    elif len(sys.argv) != 1:
        sys.exit("uso: fnv_ref.py [producer_id sequence]")
    print("orden de bytes del host: %s" % sys.byteorder)
    for pid, seq in pairs:
        print("producer_id=%-10d sequence=%-20d checksum=0x%08x fill=%s"
              % (pid, seq, checksum(pid, seq), list(fill(pid, seq))))
