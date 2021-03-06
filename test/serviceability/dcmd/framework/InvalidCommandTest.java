/*
 * Copyright (c) 2015, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

import com.oracle.java.testlibrary.OutputAnalyzer;
import com.oracle.java.testlibrary.dcmd.CommandExecutor;
import com.oracle.java.testlibrary.dcmd.PidJcmdExecutor;
import com.oracle.java.testlibrary.dcmd.MainClassJcmdExecutor;
import com.oracle.java.testlibrary.dcmd.FileJcmdExecutor;
import com.oracle.java.testlibrary.dcmd.JMXExecutor;
import org.testng.annotations.Test;

/*
 * @test
 * @summary Test of invalid diagnostic command (tests all DCMD executors)
 * @library /testlibrary
 * @ignore 8072440
 * @build com.oracle.java.testlibrary.*
 * @build com.oracle.java.testlibrary.dcmd.*
 * @run testng/othervm -XX:+UsePerfData InvalidCommandTest
 */
public class InvalidCommandTest {

    public void run(CommandExecutor executor) {
        OutputAnalyzer output = executor.execute("asdf");
        output.shouldContain("Unknown diagnostic command");
    }

    @Test
    public void pid() {
        run(new PidJcmdExecutor());
    }

    @Test
    public void mainClass() {
        run(new MainClassJcmdExecutor());
    }

    @Test
    public void file() {
        run(new FileJcmdExecutor());
    }

    @Test
    public void jmx() {
        run(new JMXExecutor());
    }
}
