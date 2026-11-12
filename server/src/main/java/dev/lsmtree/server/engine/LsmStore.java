package dev.lsmtree.server.engine;

import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.invoke.MethodHandle;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.locks.ReentrantLock;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_BYTE;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;

/**
 * Java view of the C++ engine, called through the Foreign Function & Memory
 * API (see bindings/lsm_c.h).
 *
 * <p>The engine is single-threaded, so every call takes one lock. Writes are
 * serialized by the engine's fsync anyway, so the lock costs little.
 */
public final class LsmStore implements AutoCloseable {

  private static final int OK = 0;
  private static final int NOT_FOUND = 1;
  private static final int INVALID_ARGUMENT = 2;

  private final MethodHandle closeHandle;
  private final MethodHandle putHandle;
  private final MethodHandle deleteHandle;
  private final MethodHandle getHandle;
  private final MethodHandle scanHandle;
  private final MethodHandle flushHandle;
  private final MethodHandle compactHandle;
  private final MethodHandle statsHandle;
  private final MethodHandle freeHandle;

  private final ReentrantLock lock = new ReentrantLock();
  private MemorySegment database; // guarded by lock; NULL once closed

  private LsmStore(SymbolLookup library, Path directory, LsmOptions options) {
    Linker linker = Linker.nativeLinker();
    MethodHandle open = linker.downcallHandle(
        find(library, "lsm_open"),
        FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_LONG, JAVA_LONG, JAVA_LONG,
            JAVA_INT, ADDRESS, ADDRESS));
    closeHandle = linker.downcallHandle(
        find(library, "lsm_close"), FunctionDescriptor.ofVoid(ADDRESS));
    putHandle = linker.downcallHandle(
        find(library, "lsm_put"),
        FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, JAVA_LONG, ADDRESS,
            JAVA_LONG, ADDRESS));
    deleteHandle = linker.downcallHandle(
        find(library, "lsm_delete"),
        FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, JAVA_LONG, ADDRESS));
    getHandle = linker.downcallHandle(
        find(library, "lsm_get"),
        FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, JAVA_LONG, ADDRESS,
            ADDRESS, ADDRESS));
    scanHandle = linker.downcallHandle(
        find(library, "lsm_scan"),
        FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, JAVA_LONG, ADDRESS,
            JAVA_LONG, ADDRESS, ADDRESS, ADDRESS));
    flushHandle = linker.downcallHandle(
        find(library, "lsm_flush"),
        FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS));
    compactHandle = linker.downcallHandle(
        find(library, "lsm_compact"),
        FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS));
    statsHandle = linker.downcallHandle(
        find(library, "lsm_get_stats"), FunctionDescriptor.ofVoid(ADDRESS, ADDRESS));
    freeHandle = linker.downcallHandle(
        find(library, "lsm_free"), FunctionDescriptor.ofVoid(ADDRESS));

    try (Arena arena = Arena.ofConfined()) {
      MemorySegment out = arena.allocate(ADDRESS);
      MemorySegment error = arena.allocate(ADDRESS);
      int status = (int) open.invokeExact(
          arena.allocateFrom(directory.toAbsolutePath().toString()),
          options.memtableBytes(), options.bloomBitsPerKey(),
          options.compactionTrigger(), options.syncWrites() ? 1 : 0, out, error);
      check(status, error);
      database = out.get(ADDRESS, 0);
    } catch (RuntimeException exception) {
      throw exception;
    } catch (Throwable throwable) {
      throw new LsmException("failed to call lsm_open", throwable);
    }
  }

  /** Loads the shared library and opens (or creates) the database. */
  public static LsmStore open(Path libraryPath, Path directory, LsmOptions options) {
    SymbolLookup library = SymbolLookup.libraryLookup(libraryPath, Arena.global());
    return new LsmStore(library, directory, options);
  }

  public void put(byte[] key, byte[] value) {
    call(arena -> {
      MemorySegment error = arena.allocate(ADDRESS);
      int status = (int) putHandle.invokeExact(database, bytes(arena, key),
          (long) key.length, bytes(arena, value), (long) value.length, error);
      check(status, error);
      return null;
    });
  }

  public void delete(byte[] key) {
    call(arena -> {
      MemorySegment error = arena.allocate(ADDRESS);
      int status = (int) deleteHandle.invokeExact(database, bytes(arena, key),
          (long) key.length, error);
      check(status, error);
      return null;
    });
  }

  public Optional<byte[]> get(byte[] key) {
    return call(arena -> {
      MemorySegment value = arena.allocate(ADDRESS);
      MemorySegment length = arena.allocate(JAVA_LONG);
      MemorySegment error = arena.allocate(ADDRESS);
      int status = (int) getHandle.invokeExact(database, bytes(arena, key),
          (long) key.length, value, length, error);
      check(status, error);
      if (status == NOT_FOUND) {
        return Optional.<byte[]>empty();
      }
      return Optional.of(take(value.get(ADDRESS, 0), length.get(JAVA_LONG, 0)));
    });
  }

  /** Returns live pairs in [begin, end) in key order; an empty end is unbounded. */
  public List<KeyValue> scan(byte[] begin, byte[] end) {
    byte[] encoded = call(arena -> {
      MemorySegment out = arena.allocate(ADDRESS);
      MemorySegment length = arena.allocate(JAVA_LONG);
      MemorySegment error = arena.allocate(ADDRESS);
      int status = (int) scanHandle.invokeExact(database, bytes(arena, begin),
          (long) begin.length, bytes(arena, end), (long) end.length, out, length,
          error);
      check(status, error);
      return take(out.get(ADDRESS, 0), length.get(JAVA_LONG, 0));
    });
    ByteBuffer buffer = ByteBuffer.wrap(encoded).order(ByteOrder.LITTLE_ENDIAN);
    long count = buffer.getLong();
    List<KeyValue> pairs = new ArrayList<>(Math.toIntExact(count));
    for (long index = 0; index < count; index++) {
      byte[] key = new byte[buffer.getInt()];
      buffer.get(key);
      byte[] value = new byte[buffer.getInt()];
      buffer.get(value);
      pairs.add(new KeyValue(key, value));
    }
    return pairs;
  }

  public void flush() {
    call(arena -> {
      MemorySegment error = arena.allocate(ADDRESS);
      check((int) flushHandle.invokeExact(database, error), error);
      return null;
    });
  }

  public void compact() {
    call(arena -> {
      MemorySegment error = arena.allocate(ADDRESS);
      check((int) compactHandle.invokeExact(database, error), error);
      return null;
    });
  }

  public LsmStats stats() {
    return call(arena -> {
      MemorySegment out = arena.allocate(JAVA_LONG, 5);
      statsHandle.invokeExact(database, out);
      return new LsmStats(out.getAtIndex(JAVA_LONG, 0), out.getAtIndex(JAVA_LONG, 1),
          out.getAtIndex(JAVA_LONG, 2), out.getAtIndex(JAVA_LONG, 3),
          out.getAtIndex(JAVA_LONG, 4));
    });
  }

  /** Closes the engine, releasing its directory lock. Safe to call twice. */
  @Override
  public void close() {
    lock.lock();
    try {
      if (database == null || database.equals(MemorySegment.NULL)) {
        return;
      }
      closeHandle.invokeExact(database);
    } catch (Throwable throwable) {
      throw new LsmException("failed to call lsm_close", throwable);
    } finally {
      database = MemorySegment.NULL;
      lock.unlock();
    }
  }

  @FunctionalInterface
  private interface NativeCall<T> {
    T apply(Arena arena) throws Throwable;
  }

  private <T> T call(NativeCall<T> body) {
    lock.lock();
    try (Arena arena = Arena.ofConfined()) {
      if (database == null || database.equals(MemorySegment.NULL)) {
        throw new IllegalStateException("LSM store is closed");
      }
      return body.apply(arena);
    } catch (RuntimeException | Error exception) {
      throw exception;
    } catch (Throwable throwable) {
      throw new LsmException("native call failed", throwable);
    } finally {
      lock.unlock();
    }
  }

  private static MemorySegment bytes(Arena arena, byte[] data) {
    // A zero-length array still needs a valid pointer for the C side.
    return data.length == 0 ? arena.allocate(1) : arena.allocateFrom(JAVA_BYTE, data);
  }

  /** Copies a native buffer into a Java array and frees the native copy. */
  private byte[] take(MemorySegment pointer, long length) throws Throwable {
    try {
      return pointer.reinterpret(length).toArray(JAVA_BYTE);
    } finally {
      freeHandle.invokeExact(pointer);
    }
  }

  private void check(int status, MemorySegment errorOut) throws Throwable {
    if (status == OK || status == NOT_FOUND) {
      return;
    }
    String message = "storage engine error " + status;
    MemorySegment pointer = errorOut.get(ADDRESS, 0);
    if (!pointer.equals(MemorySegment.NULL)) {
      try {
        message = pointer.reinterpret(Long.MAX_VALUE).getString(0);
      } finally {
        freeHandle.invokeExact(pointer);
      }
    }
    if (status == INVALID_ARGUMENT) {
      throw new LsmInvalidArgumentException(message);
    }
    throw new LsmException(message);
  }

  private static MemorySegment find(SymbolLookup library, String name) {
    return library.find(name)
        .orElseThrow(() -> new LsmException("symbol not found in engine library: " + name));
  }
}
