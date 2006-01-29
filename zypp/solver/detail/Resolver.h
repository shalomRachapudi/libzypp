/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* Resolver.h
 *
 * Copyright (C) 2000-2002 Ximian, Inc.
 * Copyright (C) 2005 SUSE Linux Products GmbH
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#ifndef ZYPP_SOLVER_DETAIL_RESOLVER_H
#define ZYPP_SOLVER_DETAIL_RESOLVER_H

#include <iosfwd>
#include <list>
#include <string>

#include "zypp/base/ReferenceCounted.h"
#include "zypp/base/PtrTypes.h"

#include "zypp/ResPool.h"

#include "zypp/solver/detail/Types.h"
#include "zypp/solver/detail/ResolverQueue.h"
#include "zypp/solver/detail/ResolverContext.h"

#include "zypp/CapSet.h"

/////////////////////////////////////////////////////////////////////////
namespace zypp
{ ///////////////////////////////////////////////////////////////////////
  ///////////////////////////////////////////////////////////////////////
  namespace solver
  { /////////////////////////////////////////////////////////////////////
    /////////////////////////////////////////////////////////////////////
    namespace detail
    { ///////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////
//
//	CLASS NAME : Resolver

class Resolver : public base::ReferenceCounted, private base::NonCopyable {

  private:
    ResPool _pool;

    int _timeout_seconds;
    bool _verifying;

    QueueItemList _initial_items;
    PoolItemList _items_to_install;
    PoolItemList _items_to_establish;
    PoolItemList _items_to_remove;
    PoolItemList _items_to_verify;

    CapSet _extra_caps;
    CapSet _extra_conflicts;

    ResolverQueueList _pending_queues;
    ResolverQueueList _pruned_queues;
    ResolverQueueList _complete_queues;
    ResolverQueueList _deferred_queues;
    ResolverQueueList _invalid_queues;

    int _valid_solution_count;

    ResolverContext_Ptr _best_context;
    bool _timed_out;

  public:

    Resolver (const ResPool & pool);
    virtual ~Resolver();

    // ---------------------------------- I/O

    friend std::ostream& operator<<(std::ostream&, const Resolver &resolver);

    // ---------------------------------- accessors

    QueueItemList initialItems () const { return _initial_items; }

    ResolverQueueList pendingQueues () const { return _pending_queues; }
    ResolverQueueList prunedQueues () const { return _pruned_queues; }
    ResolverQueueList completeQueues () const { return _complete_queues; }
    ResolverQueueList deferredQueues () const { return _deferred_queues; }
    ResolverQueueList invalidQueues () const { return _invalid_queues; }

    ResolverContext_Ptr bestContext (void) const { return _best_context; }

    // ---------------------------------- methods

    void setTimeout (int seconds) { _timeout_seconds = seconds; }

    ResPool pool (void) const;
    void setPool (const ResPool & pool) { _pool = pool; }

//    void setCurrentChannel (Channel_constPtr channel) { _current_channel = channel; }
//    void addSubscribedChannel (Channel_constPtr channel);

    void addPoolItemToInstall (PoolItem_Ref item);
    void addPoolItemsToInstallFromList (PoolItemList & rl);

    void addPoolItemToRemove (PoolItem_Ref item);
    void addPoolItemsToRemoveFromList (PoolItemList & rl);

    void addPoolItemToEstablish (PoolItem_Ref item);
    void addPoolItemsToEstablishFromList (PoolItemList & rl);

    void addPoolItemToVerify (PoolItem_Ref item);

    void addExtraCapability (const Capability & capability);
    void addExtraConflict (const Capability & capability);

    void verifySystem (void);
    void establishState (const ResolverContext_Ptr context = NULL);
    bool resolveDependencies (const ResolverContext_Ptr context = NULL);

    ResolverProblemList problems (void) const;
    bool applySolutions (const ProblemSolutionList &solutions);

    void reset (void);
};

///////////////////////////////////////////////////////////////////
    };// namespace detail
    /////////////////////////////////////////////////////////////////////
    /////////////////////////////////////////////////////////////////////
  };// namespace solver
  ///////////////////////////////////////////////////////////////////////
  ///////////////////////////////////////////////////////////////////////
};// namespace zypp
/////////////////////////////////////////////////////////////////////////

#endif // ZYPP_SOLVER_DETAIL_RESOLVER_H
