package dev.lsmtree.server.api;

import dev.lsmtree.server.engine.KeyValue;
import dev.lsmtree.server.engine.LsmStore;
import io.swagger.v3.oas.annotations.Operation;
import io.swagger.v3.oas.annotations.tags.Tag;
import java.nio.charset.StandardCharsets;
import java.util.List;
import org.springframework.http.MediaType;
import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.DeleteMapping;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PathVariable;
import org.springframework.web.bind.annotation.PutMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RequestParam;
import org.springframework.web.bind.annotation.RestController;

@RestController
@RequestMapping("/api/v1/kv")
@Tag(name = "Key-value", description = "Read, write, delete and scan keys")
public class KvController {

  static final int MAX_SCAN_LIMIT = 10_000;
  private static final MediaType TEXT = new MediaType("text", "plain", StandardCharsets.UTF_8);

  private final LsmStore store;

  public KvController(LsmStore store) {
    this.store = store;
  }

  @Operation(summary = "Write a value",
      description = "Returns 204 once the write is durable in the write-ahead log.")
  @PutMapping(path = "/{key}", consumes = MediaType.ALL_VALUE)
  public ResponseEntity<Void> put(@PathVariable String key,
      @RequestBody(required = false) byte[] value) {
    store.put(utf8(key), value == null ? new byte[0] : value);
    return ResponseEntity.noContent().build();
  }

  @Operation(summary = "Read a value", description = "Returns 404 if the key is absent or deleted.")
  @GetMapping(path = "/{key}", produces = MediaType.TEXT_PLAIN_VALUE)
  public ResponseEntity<byte[]> get(@PathVariable String key) {
    return store.get(utf8(key))
        .map(value -> ResponseEntity.ok().contentType(TEXT).body(value))
        .orElseGet(() -> ResponseEntity.notFound().build());
  }

  @Operation(summary = "Delete a key", description = "Idempotent: deleting an absent key succeeds.")
  @DeleteMapping("/{key}")
  public ResponseEntity<Void> delete(@PathVariable String key) {
    store.delete(utf8(key));
    return ResponseEntity.noContent().build();
  }

  @Operation(summary = "Scan a key range",
      description = "Returns live keys in [from, to) in key order. An empty 'to' "
          + "means no upper bound. At most 'limit' items are returned.")
  @GetMapping(produces = MediaType.APPLICATION_JSON_VALUE)
  public ScanResponse scan(@RequestParam(defaultValue = "") String from,
      @RequestParam(defaultValue = "") String to,
      @RequestParam(defaultValue = "1000") int limit) {
    if (limit < 1 || limit > MAX_SCAN_LIMIT) {
      throw new IllegalArgumentException("limit must be between 1 and " + MAX_SCAN_LIMIT);
    }
    List<KeyValue> pairs = store.scan(utf8(from), utf8(to));
    List<ScanResponse.Item> items = pairs.stream()
        .limit(limit)
        .map(pair -> new ScanResponse.Item(
            new String(pair.key(), StandardCharsets.UTF_8),
            new String(pair.value(), StandardCharsets.UTF_8)))
        .toList();
    return new ScanResponse(items, pairs.size() > limit);
  }

  private static byte[] utf8(String text) {
    return text.getBytes(StandardCharsets.UTF_8);
  }

  /** Scan result; {@code truncated} is true when more keys matched than were returned. */
  public record ScanResponse(List<Item> items, boolean truncated) {
    public record Item(String key, String value) {}
  }
}
