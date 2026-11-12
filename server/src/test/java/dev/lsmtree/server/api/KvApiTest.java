package dev.lsmtree.server.api;

import static org.hamcrest.Matchers.containsString;
import static org.hamcrest.Matchers.greaterThanOrEqualTo;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.delete;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.get;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.post;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.put;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.content;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.jsonPath;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.status;

import java.nio.file.Path;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.boot.test.context.SpringBootTest;
import org.springframework.http.MediaType;
import org.springframework.test.context.DynamicPropertyRegistry;
import org.springframework.test.context.DynamicPropertySource;
import org.springframework.test.web.servlet.MockMvc;
import org.springframework.test.web.servlet.setup.MockMvcBuilders;
import org.springframework.web.context.WebApplicationContext;

/** End-to-end HTTP tests: Spring MVC through FFM into the real engine. */
@SpringBootTest
class KvApiTest {

  @TempDir
  static Path dataDir;

  @DynamicPropertySource
  static void engineProperties(DynamicPropertyRegistry registry) {
    registry.add("lsm.data-dir", () -> dataDir.toString());
    registry.add("lsm.sync-writes", () -> "false");
  }

  @Autowired
  WebApplicationContext context;

  MockMvc mvc;

  @BeforeEach
  void setUp() {
    mvc = MockMvcBuilders.webAppContextSetup(context).build();
  }

  @Test
  void putGetDeleteRoundTrip() throws Exception {
    mvc.perform(put("/api/v1/kv/user:1").contentType(MediaType.TEXT_PLAIN).content("Ada"))
        .andExpect(status().isNoContent());
    mvc.perform(get("/api/v1/kv/user:1"))
        .andExpect(status().isOk())
        .andExpect(content().string("Ada"));
    mvc.perform(delete("/api/v1/kv/user:1")).andExpect(status().isNoContent());
    mvc.perform(get("/api/v1/kv/user:1")).andExpect(status().isNotFound());
  }

  @Test
  void scanReturnsRangeAndTruncationFlag() throws Exception {
    for (String key : new String[] {"scan:a", "scan:b", "scan:c", "other:x"}) {
      mvc.perform(put("/api/v1/kv/" + key).content("v-" + key))
          .andExpect(status().isNoContent());
    }
    mvc.perform(get("/api/v1/kv").param("from", "scan:").param("to", "scan;"))
        .andExpect(status().isOk())
        .andExpect(jsonPath("$.items.length()").value(3))
        .andExpect(jsonPath("$.items[0].key").value("scan:a"))
        .andExpect(jsonPath("$.items[2].value").value("v-scan:c"))
        .andExpect(jsonPath("$.truncated").value(false));
    mvc.perform(get("/api/v1/kv").param("from", "scan:").param("to", "scan;")
            .param("limit", "2"))
        .andExpect(jsonPath("$.items.length()").value(2))
        .andExpect(jsonPath("$.truncated").value(true));
  }

  @Test
  void badInputReturnsProblemDetails() throws Exception {
    mvc.perform(get("/api/v1/kv").param("from", "z").param("to", "a"))
        .andExpect(status().isBadRequest())
        .andExpect(jsonPath("$.detail").value(containsString("scan end precedes begin")));
    mvc.perform(get("/api/v1/kv").param("limit", "0"))
        .andExpect(status().isBadRequest());
  }

  @Test
  void adminEndpointsFlushCompactAndReportStats() throws Exception {
    mvc.perform(put("/api/v1/kv/admin:k").content("v")).andExpect(status().isNoContent());
    mvc.perform(post("/api/v1/admin/flush")).andExpect(status().isNoContent());
    mvc.perform(post("/api/v1/admin/compact")).andExpect(status().isNoContent());
    mvc.perform(get("/api/v1/admin/stats"))
        .andExpect(status().isOk())
        .andExpect(jsonPath("$.sstableCount").value(greaterThanOrEqualTo(1)))
        .andExpect(jsonPath("$.memtableEntries").value(0));
  }

  @Test
  void demoLoadWritesKeysAndStatsReportLevels() throws Exception {
    mvc.perform(post("/api/v1/demo/load").param("count", "300"))
        .andExpect(status().isOk())
        .andExpect(jsonPath("$.written").value(300));
    mvc.perform(get("/api/v1/kv").param("from", "demo:").param("to", "demo;"))
        .andExpect(status().isOk())
        .andExpect(jsonPath("$.items.length()").value(greaterThanOrEqualTo(1)));
    mvc.perform(get("/api/v1/admin/stats"))
        .andExpect(jsonPath("$.tablesPerLevel.length()").value(7))
        .andExpect(jsonPath("$.sequence").value(greaterThanOrEqualTo(300)));
    mvc.perform(post("/api/v1/demo/read").param("count", "200"))
        .andExpect(status().isOk())
        .andExpect(jsonPath("$.read").value(200))
        .andExpect(jsonPath("$.found").value(greaterThanOrEqualTo(1)));
    mvc.perform(post("/api/v1/demo/load").param("count", "0"))
        .andExpect(status().isBadRequest());
  }

  @Test
  void dashboardIsServedAtTheRoot() throws Exception {
    mvc.perform(get("/index.html"))
        .andExpect(status().isOk())
        .andExpect(content().string(containsString("The tree, live")));
    mvc.perform(get("/assets/app.js")).andExpect(status().isOk());
    mvc.perform(get("/assets/app.css")).andExpect(status().isOk());
  }

  @Test
  void metricsAndApiDocsArePublished() throws Exception {
    mvc.perform(get("/actuator/metrics/lsm.sstables")).andExpect(status().isOk());
    mvc.perform(get("/actuator/health"))
        .andExpect(status().isOk())
        .andExpect(jsonPath("$.status").value("UP"));
    mvc.perform(get("/v3/api-docs"))
        .andExpect(status().isOk())
        .andExpect(jsonPath("$.info.title").value("LSM-tree key-value API"));
  }
}
