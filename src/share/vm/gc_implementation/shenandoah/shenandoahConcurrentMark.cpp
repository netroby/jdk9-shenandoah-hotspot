/*
 * Copyright (c) 2013, Oracle and/or its affiliates. All rights reserved.
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
 *
 */

#include "gc_implementation/shenandoah/shenandoahConcurrentMark.hpp"

CMRootRegionScanTask::CMRootRegionScanTask(ShenandoahConcurrentMark* cm) :
  AbstractGangTask("Root Region Scan"), _cm(cm) { }

void CMRootRegionScanTask::work(uint worker_id) {
}

CMConcurrentMarkingTask::CMConcurrentMarkingTask(ShenandoahConcurrentMark* cm) :
  AbstractGangTask("Root Region Scan"), _cm(cm) { }

void CMConcurrentMarkingTask::work(uint worker_id) {
}

ShenandoahConcurrentMark::ShenandoahConcurrentMark() :

  _max_worker_id(MAX2((uint)ParallelGCThreads, 1U)),
  _task_queues(new CMTaskQueueSet((int) _max_worker_id)),
  _terminator(ParallelTaskTerminator((int) _max_worker_id, _task_queues))

{}

void ShenandoahConcurrentMark::scanRootRegions() {

  CMRootRegionScanTask task(this);
  task.work(0);
}

void ShenandoahConcurrentMark::markFromRoots() {

  CMConcurrentMarkingTask markingTask(this);
  markingTask.work(0);
}
