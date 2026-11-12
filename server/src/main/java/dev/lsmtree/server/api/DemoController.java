package dev.lsmtree.server.api;

import dev.lsmtree.server.engine.LsmStore;
import io.swagger.v3.oas.annotations.Operation;
import io.swagger.v3.oas.annotations.tags.Tag;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ThreadLocalRandom;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RequestParam;
import org.springframework.web.bind.annotation.RestController;

/** Generates load so the dashboard can show flushes and compactions live. */
@RestController
@RequestMapping("/api/v1/demo")
@Tag(name = "Demo", description = "Load generation for the dashboard")
public class DemoController {

  static final int MAX_LOAD = 100_000;
  private static final String WORDS =
      "amber birch cedar delta ember fjord grove harbor iris juniper kestrel lumen "
          + "maple nova orbit pine quartz river sable tundra umber vale willow xenon";

  private final LsmStore store;

  public DemoController(LsmStore store) {
    this.store = store;
  }

  @Operation(summary = "Write random keys",
      description = "Writes `count` keys named demo:000000.. with short random "
          + "values (overwriting some), so memtables flush and tables compact.")
  @PostMapping("/load")
  public LoadResult load(@RequestParam(defaultValue = "5000") int count) {
    if (count < 1 || count > MAX_LOAD) {
      throw new IllegalArgumentException("count must be between 1 and " + MAX_LOAD);
    }
    String[] words = WORDS.split(" ");
    ThreadLocalRandom random = ThreadLocalRandom.current();
    long started = System.nanoTime();
    for (int index = 0; index < count; index++) {
      // Keys spread over twice the count, so some writes overwrite.
      String key = String.format("demo:%06d", random.nextInt(count * 2));
      String value = words[random.nextInt(words.length)] + "-" + words[random.nextInt(words.length)]
          + "-" + random.nextInt(1_000_000);
      store.put(key.getBytes(StandardCharsets.UTF_8), value.getBytes(StandardCharsets.UTF_8));
    }
    long millis = (System.nanoTime() - started) / 1_000_000;
    return new LoadResult(count, millis);
  }

  @Operation(summary = "Read random keys",
      description = "Looks up `count` random demo keys, about half of which do not "
          + "exist, so Bloom filters and the block cache have work to show.")
  @PostMapping("/read")
  public ReadResult read(@RequestParam(defaultValue = "5000") int count) {
    if (count < 1 || count > MAX_LOAD) {
      throw new IllegalArgumentException("count must be between 1 and " + MAX_LOAD);
    }
    ThreadLocalRandom random = ThreadLocalRandom.current();
    int found = 0;
    long started = System.nanoTime();
    for (int index = 0; index < count; index++) {
      String key = String.format("demo:%06d", random.nextInt(count * 2));
      if (store.get(key.getBytes(StandardCharsets.UTF_8)).isPresent()) {
        found++;
      }
    }
    long millis = (System.nanoTime() - started) / 1_000_000;
    return new ReadResult(count, found, millis);
  }

  /** How many keys were written and how long it took. */
  public record LoadResult(int written, long millis) {}

  /** How many keys were looked up, how many existed, and how long it took. */
  public record ReadResult(int read, int found, long millis) {}
}
