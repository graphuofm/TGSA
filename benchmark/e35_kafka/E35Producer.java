// E35: produce a TGSA corpus (.bin of packed 20-byte records) into a Kafka topic with the
// Java producer defaults except acks (set on the command line), one KafkaProducer per thread,
// 20-byte binary values, null keys (default sticky partitioner). Prints one JSON line.
import java.io.*;
import java.nio.*;
import java.nio.channels.FileChannel;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;
import org.apache.kafka.clients.producer.*;
import org.apache.kafka.common.serialization.ByteArraySerializer;

public class E35Producer {
    static final int REC = 20;

    public static void main(String[] a) throws Exception {
        Map<String,String> arg = new HashMap<>();
        for (int i = 0; i + 1 < a.length; i += 2) arg.put(a[i].replaceFirst("^--", ""), a[i+1]);
        final String bin = arg.get("bin");
        final String bootstrap = arg.getOrDefault("bootstrap", "127.0.0.1:9092");
        final String topic = arg.get("topic");
        final int threads = Integer.parseInt(arg.getOrDefault("threads", "8"));
        final String acks = arg.getOrDefault("acks", "1");
        final long limit = Long.parseLong(arg.getOrDefault("limit", "0"));
        final String linger = arg.get("linger-ms");      // null = producer default (0)
        final String batchSize = arg.get("batch-size");  // null = producer default (16384)

        final long bytes = Files.size(Paths.get(bin));
        if (bytes % REC != 0) throw new IllegalStateException("bad .bin size");
        long n = bytes / REC;
        if (limit > 0 && limit < n) n = limit;
        final long total = n;

        final CyclicBarrier start = new CyclicBarrier(threads + 1);
        final CountDownLatch done = new CountDownLatch(threads);
        final AtomicLong sent = new AtomicLong(), errors = new AtomicLong();
        final long[] tEnd = new long[threads];

        for (int t = 0; t < threads; t++) {
            final int tid = t;
            final long lo = total * tid / threads, hi = total * (tid + 1) / threads;
            new Thread(() -> {
                try (FileChannel ch = FileChannel.open(Paths.get(bin), StandardOpenOption.READ)) {
                    MappedByteBuffer mb = ch.map(FileChannel.MapMode.READ_ONLY, lo * REC, (hi - lo) * REC);
                    Properties p = new Properties();
                    p.put(ProducerConfig.BOOTSTRAP_SERVERS_CONFIG, bootstrap);
                    p.put(ProducerConfig.ACKS_CONFIG, acks);
                    p.put(ProducerConfig.ENABLE_IDEMPOTENCE_CONFIG, "false");
                    p.put(ProducerConfig.KEY_SERIALIZER_CLASS_CONFIG, ByteArraySerializer.class.getName());
                    p.put(ProducerConfig.VALUE_SERIALIZER_CLASS_CONFIG, ByteArraySerializer.class.getName());
                    p.put(ProducerConfig.CLIENT_ID_CONFIG, "e35-producer-" + tid);
                    if (linger != null) p.put(ProducerConfig.LINGER_MS_CONFIG, linger);
                    if (batchSize != null) p.put(ProducerConfig.BATCH_SIZE_CONFIG, batchSize);
                    KafkaProducer<byte[],byte[]> prod = new KafkaProducer<>(p);
                    start.await();
                    long mine = 0;
                    Callback cb = (md, ex) -> { if (ex != null) errors.incrementAndGet(); };
                    for (long i = lo; i < hi; i++) {
                        byte[] v = new byte[REC];
                        mb.get(v);
                        prod.send(new ProducerRecord<>(topic, null, v), cb);
                        mine++;
                    }
                    prod.flush();
                    prod.close();
                    tEnd[tid] = System.nanoTime();
                    sent.addAndGet(mine);
                } catch (Exception e) {
                    e.printStackTrace();
                    errors.addAndGet(hi - lo);
                } finally { done.countDown(); }
            }, "e35-prod-" + tid).start();
        }
        start.await();
        final long t0 = System.nanoTime();
        done.await();
        long t1 = t0;
        for (long te : tEnd) t1 = Math.max(t1, te);
        double sec = (t1 - t0) / 1e9;
        System.out.printf(Locale.ROOT,
            "{\"events\":%d,\"sent\":%d,\"errors\":%d,\"threads\":%d,\"acks\":\"%s\",\"linger_ms\":\"%s\",\"batch_size\":\"%s\",\"seconds\":%.6f,\"rate\":%.1f}%n",
            total, sent.get(), errors.get(), threads, acks, linger == null ? "default" : linger,
            batchSize == null ? "default" : batchSize, sec, sent.get() / sec);
    }
}
