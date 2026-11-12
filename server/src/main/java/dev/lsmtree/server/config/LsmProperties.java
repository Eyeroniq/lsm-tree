package dev.lsmtree.server.config;

import java.nio.file.Path;
import org.springframework.boot.context.properties.ConfigurationProperties;
import org.springframework.boot.context.properties.bind.DefaultValue;

/**
 * Settings under the {@code lsm.} prefix; environment variables such as
 * {@code LSM_DATA_DIR} and {@code LSM_LIBRARY_PATH} also bind here.
 *
 * @param dataDir directory holding the WAL, SSTables and LOCK file
 * @param libraryPath engine shared library; when unset, {@code build/} and
 *     {@code ../build/} under the working directory are searched
 * @param tableBytes target size of compacted tables; 0 keeps the default
 * @param level1Bytes byte budget of level 1; 0 keeps the default
 */
@ConfigurationProperties("lsm")
public record LsmProperties(
    @DefaultValue("data") Path dataDir,
    Path libraryPath,
    @DefaultValue("65536") long memtableBytes,
    @DefaultValue("10") long bloomBitsPerKey,
    @DefaultValue("4") long compactionTrigger,
    @DefaultValue("0") long tableBytes,
    @DefaultValue("0") long level1Bytes,
    @DefaultValue("true") boolean syncWrites) {}
