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
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"


SCMRootRegionScanTask::SCMRootRegionScanTask(ShenandoahConcurrentMark* cm) :
  AbstractGangTask("Root Region Scan"), _cm(cm) { }

void SCMRootRegionScanTask::work(uint worker_id) {
}

SCMConcurrentMarkingTask::SCMConcurrentMarkingTask(ShenandoahConcurrentMark* cm) :
  AbstractGangTask("Root Region Scan"), _cm(cm) { }

void SCMConcurrentMarkingTask::work(uint worker_id) {
  tty->print("SCMConcurrentMarkingTask work worker = %d\n", worker_id);
  Thread::current()->print_on(tty);
  ShenandoahHeap* sh = (ShenandoahHeap *) Universe::heap();
  sh->do_concurrent_marking();
}

ShenandoahConcurrentMark::ShenandoahConcurrentMark() :

  _max_worker_id(MAX2((uint)ParallelGCThreads, 1U)),
  _task_queues(new SCMTaskQueueSet((int) _max_worker_id)),
  _terminator(ParallelTaskTerminator((int) _max_worker_id, _task_queues))

{}

void ShenandoahConcurrentMark::scanRootRegions() {

  SCMRootRegionScanTask task(this);
  task.work(0);
}

void ShenandoahConcurrentMark::markFromRoots() {
  tty->print("Starting markFromRoots");
  //  SCMConcurrentMarkingTask markingTask(this);
  //  markingTask.work(0);
  ShenandoahHeap* sh = (ShenandoahHeap *) Universe::heap();
  sh->do_concurrent_marking();
}

void ShenandoahConcurrentMark::finishMarkFromRoots() {
  SCMConcurrentMarkingTask markingTask(this);
  markingTask.work(0);
}

void ShenandoahConcurrentMark::checkpointRootsFinal() {
  SCMConcurrentMarkingTask markingTask(this);
  markingTask.work(0);
}
