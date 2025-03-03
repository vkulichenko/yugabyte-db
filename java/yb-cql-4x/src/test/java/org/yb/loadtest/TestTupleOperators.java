// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
package org.yb.loadtest;

import org.apache.spark.SparkConf;

import org.junit.Test;
import org.junit.runner.RunWith;
import static org.yb.AssertionWrappers.assertTrue;
import static org.yb.AssertionWrappers.assertEquals;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.net.InetSocketAddress;
import java.util.Iterator;
import java.util.List;

import org.yb.minicluster.BaseMiniClusterTest;
import org.yb.util.YBTestRunnerNonTsanOnly;

import com.datastax.spark.connector.cql.CassandraConnector;

import com.datastax.oss.driver.api.core.CqlSession;
import com.datastax.oss.driver.api.core.cql.ResultSet;
import com.datastax.oss.driver.api.core.cql.PreparedStatement;
import com.datastax.oss.driver.api.core.cql.BoundStatement;
import com.datastax.oss.driver.api.core.cql.Row;

import com.datastax.oss.driver.api.core.type.DataTypes;
import com.datastax.oss.driver.api.core.type.TupleType;

@RunWith(value = YBTestRunnerNonTsanOnly.class)
public class TestTupleOperators extends BaseMiniClusterTest {

    private static Logger logger = LoggerFactory.getLogger(TestTupleOperators.class);
    private static String KEYSPACE = "test";

    @Test
    public void testTupleBind() throws Exception {
        // Set up config.
        List<InetSocketAddress> addresses = miniCluster.getCQLContactPoints();

        // Setup the local spark master
        SparkConf conf = new SparkConf().setAppName("yb.spark-jsonb")
                .setMaster("local[1]")
                .set("spark.cassandra.connection.localDC", "datacenter1")
                .set("spark.cassandra.connection.host", addresses.get(0).getHostName())
                .set("spark.sql.catalog.mycatalog",
                        "com.datastax.spark.connector.datasource.CassandraCatalog");

        CassandraConnector connector = CassandraConnector.apply(conf);

        // Create a Cassandra session, and initialize the keyspace.
        CqlSession session = connector.openSession();

        String createKeyspace = "CREATE KEYSPACE IF NOT EXISTS " + KEYSPACE + ";";
        session.execute(createKeyspace);

        String tbl = KEYSPACE + ".test_tuple_bind";
        session.execute("CREATE TABLE " + tbl +
                " (h1 INT, h2 TEXT," +
                "  r1 INT, r2 TEXT," +
                "  v1 INT, v2 TEXT, PRIMARY KEY ((h1, h2), r1, r2))" +
                "  WITH transactions = {'enabled': 'true'}");

        // Insert data.
        int insert_row_count = 10;
        String stmt = String.format("INSERT INTO %s(h1, h2, r1, r2, v1, v2) " +
                "VALUES(?, ?, ?, ?, ?, ?)", tbl);
        PreparedStatement insertStmt = session.prepare(stmt);
        for (int idx = 0; idx < insert_row_count; idx++) {
            session.execute(insertStmt.bind(idx, "h" + idx,
                    idx + 100, "r" + (idx + 100),
                    idx + 1000, "v" + (idx + 1000)));
        }

        // Simple SELECT to verify that this setup works.
        String selectStmt = String.format("SELECT h1, h2, r1, r2, v1, v2 FROM %s" +
                "  WHERE h1 = 7 AND h2 = 'h7' AND r1 = 107;", tbl);
        ResultSet rs = session.execute(selectStmt);

        int select_row_count = 0;
        Iterator<Row> iter = rs.iterator();
        while (iter.hasNext()) {
            Row row = iter.next();
            assertEquals(row.getInt(0), 7);
            assertEquals(row.getString(1), "h7");
            assertEquals(row.getInt(2), 107);
            assertEquals(row.getString(3), "r107");
            assertEquals(row.getInt(4), 1007);
            assertEquals(row.getString(5), "v1007");
            select_row_count++;
        }
        assertEquals(select_row_count, 1);

        // Unsupported bind format test #1
        selectStmt = String.format("SELECT * FROM %s WHERE (r1, r2) IN (:tup1, :tup2);", tbl);
        PreparedStatement preparedSelect;
        try {
            preparedSelect = session.prepare(selectStmt);
        } catch (com.datastax.oss.driver.api.core.servererrors.SyntaxError e) {
            assertTrue(e.getMessage().contains("Feature Not Supported. Bind format not supported"));
            logger.info("Expected exception", e);
        }

        // Use the below tests once the format is supported.
        // TupleType tupleType = DataTypes.tupleOf(DataTypes.INT, DataTypes.TEXT);
        // BoundStatement boundStmt = preparedSelect.boundStatementBuilder()
        // .setTupleValue("tup1", tupleType.newValue(101, "r101"))
        // .setTupleValue("tup2", tupleType.newValue(102, "r102"))
        // .build();
        // rs = session.execute(boundStmt);

        // select_row_count = 0;
        // iter = rs.iterator();
        // int counter = 1;
        // while (iter.hasNext()) {
        // Row row = iter.next();
        // String result = String.format("Result = %d, %s, %d, %s, %d, %s",
        // row.getInt(0),
        // row.getString(1),
        // row.getInt(2),
        // row.getString(3),
        // row.getInt(4),
        // row.getString(5));
        // logger.info(result);

        // assertEquals(row.getInt(0), counter); /* h1 */
        // assertEquals(row.getInt(2), counter + 100); /* r1 */
        // assertEquals(row.getInt(4), counter + 1000); /* v1 */

        // select_row_count++;
        // counter++;
        // }
        // assertEquals(select_row_count, 2);

        // Unsupported bind format test #2
        selectStmt = String.format("SELECT * FROM %s WHERE (r1, r2) IN ?;", tbl);
        try {
            preparedSelect = session.prepare(selectStmt);
        } catch (com.datastax.oss.driver.api.core.servererrors.SyntaxError e) {
            assertTrue(e.getMessage().contains("Feature Not Supported. Bind format not supported"));
            logger.info("Expected exception", e);
        }
    }
}
