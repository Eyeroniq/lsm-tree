package dev.lsmtree.server.api;

import dev.lsmtree.server.engine.LsmStats;
import dev.lsmtree.server.engine.LsmStore;
import io.swagger.v3.oas.annotations.Operation;
import io.swagger.v3.oas.annotations.tags.Tag;
import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RestController;

@RestController
@RequestMapping("/api/v1/admin")
@Tag(name = "Admin", description = "Engine maintenance and statistics")
public class AdminController {

  private final LsmStore store;

  public AdminController(LsmStore store) {
    this.store = store;
  }

  @Operation(summary = "Flush the memtable to a new SSTable")
  @PostMapping("/flush")
  public ResponseEntity<Void> flush() {
    store.flush();
    return ResponseEntity.noContent().build();
  }

  @Operation(summary = "Merge all SSTables into one")
  @PostMapping("/compact")
  public ResponseEntity<Void> compact() {
    store.compact();
    return ResponseEntity.noContent().build();
  }

  @Operation(summary = "Engine counters")
  @GetMapping("/stats")
  public LsmStats stats() {
    return store.stats();
  }
}
