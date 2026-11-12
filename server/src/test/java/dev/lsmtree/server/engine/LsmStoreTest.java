package dev.lsmtree.server.engine;

import static org.assertj.core.api.Assertions.assertThat;
import static org.assertj.core.api.Assertions.assertThatThrownBy;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

/** Exercises the FFM binding directly against the real engine library. */
class LsmStoreTest {

  @TempDir
  Path directory;

  static Path library() {
    Path path = Path.of("..", "build", System.mapLibraryName("lsm"));
    assertThat(path).as("run 'make shared' before the Java tests").exists();
    return path;
  }

  static LsmStore open(Path directory) {
    return LsmStore.open(library(), directory, LsmOptions.defaults());
  }

  static byte[] b(String text) {
    return text.getBytes(StandardCharsets.UTF_8);
  }

  static String s(byte[] bytes) {
    return new String(bytes, StandardCharsets.UTF_8);
  }

  @Test
  void putGetAndDelete() {
    try (LsmStore store = open(directory)) {
      store.put(b("alpha"), b("one"));
      assertThat(store.get(b("alpha"))).map(LsmStoreTest::s).contains("one");
      assertThat(store.get(b("missing"))).isEmpty();
      store.delete(b("alpha"));
      assertThat(store.get(b("alpha"))).isEmpty();
    }
  }

  @Test
  void emptyAndUnicodeValuesRoundTrip() {
    try (LsmStore store = open(directory)) {
      store.put(b("empty"), new byte[0]);
      store.put(b("ключ"), b("значение ✓"));
      assertThat(store.get(b("empty"))).hasValueSatisfying(v -> assertThat(v).isEmpty());
      assertThat(store.get(b("ключ"))).map(LsmStoreTest::s).contains("значение ✓");
    }
  }

  @Test
  void valuesSurviveFlushCompactAndReopen() {
    try (LsmStore store = open(directory)) {
      store.put(b("a"), b("1"));
      store.flush();
      store.put(b("b"), b("2"));
      store.flush();
      store.put(b("a"), b("updated"));
      store.compact();
      assertThat(store.stats().sstableCount()).isEqualTo(1);
    }
    try (LsmStore store = open(directory)) {
      assertThat(store.get(b("a"))).map(LsmStoreTest::s).contains("updated");
      assertThat(store.get(b("b"))).map(LsmStoreTest::s).contains("2");
    }
  }

  @Test
  void scanReturnsHalfOpenRangeInKeyOrder() {
    try (LsmStore store = open(directory)) {
      for (String key : List.of("d", "a", "c", "b", "e")) {
        store.put(b(key), b("v-" + key));
      }
      store.flush();
      store.delete(b("c"));
      List<KeyValue> pairs = store.scan(b("b"), b("e"));
      assertThat(pairs).extracting(pair -> s(pair.key())).containsExactly("b", "d");
      assertThat(pairs).extracting(pair -> s(pair.value())).containsExactly("v-b", "v-d");
      assertThat(store.scan(b(""), b(""))).hasSize(4);
    }
  }

  @Test
  void secondOpenOfSameDirectoryIsRejected() {
    try (LsmStore store = open(directory)) {
      store.put(b("k"), b("v"));
      assertThatThrownBy(() -> open(directory))
          .isInstanceOf(LsmException.class)
          .hasMessageContaining("already open");
    }
    try (LsmStore reopened = open(directory)) {
      assertThat(reopened.get(b("k"))).isPresent();
    }
  }

  @Test
  void invalidInputBecomesInvalidArgument() {
    try (LsmStore store = open(directory)) {
      assertThatThrownBy(() -> store.put(new byte[0], b("v")))
          .isInstanceOf(LsmInvalidArgumentException.class);
      assertThatThrownBy(() -> store.scan(b("z"), b("a")))
          .isInstanceOf(LsmInvalidArgumentException.class);
    }
  }

  @Test
  void closedStoreRejectsCalls() {
    LsmStore store = open(directory);
    store.close();
    store.close(); // Second close is a no-op.
    assertThatThrownBy(() -> store.get(b("k"))).isInstanceOf(IllegalStateException.class);
  }

  @Test
  void concurrentWritersAreSerialized() throws Exception {
    LsmOptions fast = new LsmOptions(4096, 10, 4, false);
    try (LsmStore store = LsmStore.open(library(), directory, fast);
        ExecutorService pool = Executors.newFixedThreadPool(8)) {
      List<Future<?>> tasks = new ArrayList<>();
      for (int thread = 0; thread < 8; thread++) {
        int id = thread;
        tasks.add(pool.submit(() -> {
          for (int index = 0; index < 250; index++) {
            store.put(b("t" + id + "-" + index), b("value-" + index));
          }
        }));
      }
      for (Future<?> task : tasks) {
        task.get();
      }
      assertThat(store.scan(b(""), b(""))).hasSize(8 * 250);
      assertThat(store.get(b("t7-249"))).map(LsmStoreTest::s).contains("value-249");
      assertThat(store.stats().sstableCount()).isPositive();
    }
    assertThat(Files.exists(directory.resolve("LOCK"))).isTrue();
  }
}
