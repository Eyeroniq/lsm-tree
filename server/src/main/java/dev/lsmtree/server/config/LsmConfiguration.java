package dev.lsmtree.server.config;

import dev.lsmtree.server.engine.LsmOptions;
import dev.lsmtree.server.engine.LsmStats;
import dev.lsmtree.server.engine.LsmStore;
import io.micrometer.core.instrument.FunctionCounter;
import io.micrometer.core.instrument.Gauge;
import io.micrometer.core.instrument.binder.MeterBinder;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import java.util.function.ToLongFunction;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;

@Configuration
@EnableConfigurationProperties(LsmProperties.class)
public class LsmConfiguration {

  @Bean(destroyMethod = "close")
  LsmStore lsmStore(LsmProperties properties) {
    LsmOptions options = new LsmOptions(properties.memtableBytes(),
        properties.bloomBitsPerKey(), properties.compactionTrigger(),
        properties.syncWrites());
    return LsmStore.open(resolveLibrary(properties.libraryPath()),
        properties.dataDir(), options);
  }

  /** Publishes the engine counters as Micrometer meters under {@code lsm.*}. */
  @Bean
  MeterBinder lsmMetrics(LsmStore store) {
    return registry -> {
      gauge("lsm.sstables", "Immutable SSTables on disk", store,
          LsmStats::sstableCount).register(registry);
      gauge("lsm.memtable.entries", "Keys in the in-memory table", store,
          LsmStats::memtableEntries).register(registry);
      gauge("lsm.sequence", "Highest sequence number written", store,
          LsmStats::sequence).register(registry);
      FunctionCounter.builder("lsm.bloom.checks", store,
              s -> read(s, LsmStats::bloomChecks))
          .description("Bloom filter probes during point reads")
          .register(registry);
      FunctionCounter.builder("lsm.bloom.negatives", store,
              s -> read(s, LsmStats::bloomNegativeHits))
          .description("Probes where the Bloom filter ruled a table out")
          .register(registry);
    };
  }

  private static Gauge.Builder<LsmStore> gauge(String name, String description,
      LsmStore store, ToLongFunction<LsmStats> field) {
    return Gauge.builder(name, store, s -> read(s, field)).description(description);
  }

  private static double read(LsmStore store, ToLongFunction<LsmStats> field) {
    try {
      return field.applyAsLong(store.stats());
    } catch (IllegalStateException closed) {
      return Double.NaN; // Store already closed during shutdown.
    }
  }

  static Path resolveLibrary(Path configured) {
    if (configured != null) {
      return configured;
    }
    String name = System.mapLibraryName("lsm");
    List<Path> candidates = List.of(Path.of("build", name), Path.of("..", "build", name));
    return candidates.stream()
        .filter(Files::isRegularFile)
        .findFirst()
        .orElseThrow(() -> new IllegalStateException(
            "Engine library " + name + " not found in " + candidates
                + "; run 'make shared' or set lsm.library-path"));
  }
}
