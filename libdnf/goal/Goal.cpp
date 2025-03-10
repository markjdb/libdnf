/*
 * Copyright (C) 2018 Red Hat, Inc.
 *
 * Licensed under the GNU Lesser General Public License Version 2.1
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <assert.h>
#include <map>
#include <vector>
#include <numeric>

extern "C" {
#include <solv/evr.h>
#include <solv/queue.h>
#include <solv/selection.h>
#include <solv/solver.h>
#include <solv/solverdebug.h>
#include <solv/testcase.h>
#include <solv/transaction.h>
#include <solv/rules.h>
}

#include "Goal-private.hpp"
#include "../hy-goal-private.hpp"
#include "../hy-iutil-private.hpp"
#include "../hy-package-private.hpp"
#include "../dnf-sack-private.hpp"
#include "../sack/packageset.hpp"
#include "../sack/query.hpp"
#include "../sack/selector.hpp"
#include "../utils/bgettext/bgettext-lib.h"
#include "../utils/tinyformat/tinyformat.hpp"
#include "IdQueue.hpp"
#include "../utils/filesystem.hpp"

namespace {

std::string pkgSolvid2str(Pool * pool, Id source)
{
    return pool_solvid2str(pool, source);
}

std::string moduleSolvid2str(Pool * pool, Id source)
{
    std::ostringstream ss;
    auto * solvable = pool_id2solvable(pool, source);
    // Add name:stream
    ss << solvable_lookup_str(solvable, SOLVABLE_DESCRIPTION);
    //Add version
    ss << ":" << pool_id2str(pool, solvable->evr);
    // Add original context
    ss << ":" << solvable_lookup_str(solvable, SOLVABLE_SUMMARY);
    ss << "." << pool_id2str(pool, solvable->arch);
    return ss.str();
}

}

namespace libdnf {

enum {NO_MATCH=1, MULTIPLE_MATCH_OBJECTS, INCORECT_COMPARISON_TYPE};

static std::map<int, const char *> ERROR_DICT = {
   {MULTIPLE_MATCH_OBJECTS, M_("Ill-formed Selector, presence of multiple match objects in the filter")},
   {INCORECT_COMPARISON_TYPE, M_("Ill-formed Selector used for the operation, incorrect comparison type")}
};

enum {RULE_DISTUPGRADE=1, RULE_INFARCH, RULE_UPDATE, RULE_JOB, RULE_JOB_UNSUPPORTED, RULE_JOB_NOTHING_PROVIDES_DEP,
    RULE_JOB_UNKNOWN_PACKAGE, RULE_JOB_PROVIDED_BY_SYSTEM, RULE_PKG, RULE_BEST_1, RULE_BEST_2,
    RULE_PKG_NOT_INSTALLABLE_1, RULE_PKG_NOT_INSTALLABLE_2, RULE_PKG_NOT_INSTALLABLE_3, RULE_PKG_NOT_INSTALLABLE_4, RULE_PKG_NOTHING_PROVIDES_DEP,
    RULE_PKG_SAME_NAME, RULE_PKG_CONFLICTS, RULE_PKG_OBSOLETES, RULE_PKG_INSTALLED_OBSOLETES, RULE_PKG_IMPLICIT_OBSOLETES,
    RULE_PKG_REQUIRES, RULE_PKG_SELF_CONFLICT, RULE_YUMOBS
};

static const std::map<int, const char *> PKG_PROBLEMS_DICT = {
    {RULE_DISTUPGRADE, M_("%s from %s  does not belong to a distupgrade repository")},
    {RULE_INFARCH, M_("%s from %s  has inferior architecture")},
    {RULE_UPDATE, M_("problem with installed package ")},
    {RULE_JOB, M_("conflicting requests")},
    {RULE_JOB_UNSUPPORTED, M_("unsupported request")},
    {RULE_JOB_NOTHING_PROVIDES_DEP, M_("nothing provides requested ")},
    {RULE_JOB_UNKNOWN_PACKAGE, M_("package %s does not exist")},
    {RULE_JOB_PROVIDED_BY_SYSTEM, M_(" is provided by the system")},
    {RULE_PKG, M_("some dependency problem")},
    {RULE_BEST_1, M_("cannot install the best update candidate for package ")},
    {RULE_BEST_2, M_("cannot install the best candidate for the job")},
    {RULE_PKG_NOT_INSTALLABLE_1, M_("package %s from %s is filtered out by modular filtering")},
    {RULE_PKG_NOT_INSTALLABLE_2, M_("package %s from %s does not have a compatible architecture")},
    {RULE_PKG_NOT_INSTALLABLE_3, M_("package %s from %s is not installable")},
    {RULE_PKG_NOT_INSTALLABLE_4, M_("package %s from %s is filtered out by exclude filtering")},
    {RULE_PKG_NOTHING_PROVIDES_DEP, M_("nothing provides %s needed by %s from %s")},
    {RULE_PKG_SAME_NAME, M_("cannot install both %s from %s and %s from %s")},
    {RULE_PKG_CONFLICTS, M_("package %s from %s conflicts with %s provided by %s from %s")},
    {RULE_PKG_OBSOLETES, M_("package %s from %s obsoletes %s provided by %s from %s")},
    {RULE_PKG_INSTALLED_OBSOLETES, M_("installed package %s obsoletes %s provided by %s from %s")},
    {RULE_PKG_IMPLICIT_OBSOLETES, M_("package %s from %s implicitly obsoletes %s provided by %s from %s")},
    {RULE_PKG_REQUIRES, M_("package %s from %s requires %s, but none of the providers can be installed")},
    {RULE_PKG_SELF_CONFLICT, M_("package %s from %s conflicts with %s provided by itself")},
    {RULE_YUMOBS, M_("both package %s from %s and %s from %s obsolete %s")}
};

static const std::map<int, const char *> MODULE_PROBLEMS_DICT = {
    {RULE_DISTUPGRADE, M_("%s from %s does not belong to a distupgrade repository")},
    {RULE_INFARCH, M_("%s from %s has inferior architecture")},
    {RULE_UPDATE, M_("problem with installed module ")},
    {RULE_JOB, M_("conflicting requests")},
    {RULE_JOB_UNSUPPORTED, M_("unsupported request")},
    {RULE_JOB_NOTHING_PROVIDES_DEP, M_("nothing provides requested ")},
    {RULE_JOB_UNKNOWN_PACKAGE, M_("module %s does not exist")},
    {RULE_JOB_PROVIDED_BY_SYSTEM, M_(" is provided by the system")},
    {RULE_PKG, M_("some dependency problem")},
    {RULE_BEST_1, M_("cannot install the best update candidate for module ")},
    {RULE_BEST_2, M_("cannot install the best candidate for the job")},
    {RULE_PKG_NOT_INSTALLABLE_1, M_("module %s from %s is disabled")},
    {RULE_PKG_NOT_INSTALLABLE_2, M_("module %s from %s does not have a compatible architecture")},
    {RULE_PKG_NOT_INSTALLABLE_3, M_("module %s from %s is not installable")},
    {RULE_PKG_NOT_INSTALLABLE_4, M_("module %s from %s is disabled")},
    {RULE_PKG_NOTHING_PROVIDES_DEP, M_("nothing provides %s needed by module %s from %s")},
    {RULE_PKG_SAME_NAME, M_("cannot install both modules %s from %s and %s from %s")},
    {RULE_PKG_CONFLICTS, M_("module %s from %s conflicts with %s provided by %s from %s")},
    {RULE_PKG_OBSOLETES, M_("module %s from %s obsoletes %s provided by %s from %s")},
    {RULE_PKG_INSTALLED_OBSOLETES, M_("installed module %s obsoletes %s provided by %s from %s")},
    {RULE_PKG_IMPLICIT_OBSOLETES, M_("module %s from %s implicitly obsoletes %s provided by %s from %s")},
    {RULE_PKG_REQUIRES, M_("module %s from %s requires %s, but none of the providers can be installed")},
    {RULE_PKG_SELF_CONFLICT, M_("module %s from %s conflicts with %s provided by itself")},
    {RULE_YUMOBS, M_("both module %s from %s and %s from %s obsolete %s")}
};

static std::string
libdnf_problemruleinfo2str(libdnf::PackageSet * modularExclude, Solver *solv, SolverRuleinfo type, Id source, Id target,
    Id dep, bool pkgs)
{
    const std::map<int, const char *> & problemDict = pkgs ? PKG_PROBLEMS_DICT : MODULE_PROBLEMS_DICT;
    const auto solvid2str = pkgs ? pkgSolvid2str : moduleSolvid2str;

    Pool * const pool = solv->pool;
    Solvable *ss;
    switch (type) {
        case SOLVER_RULE_DISTUPGRADE:
            return tfm::format(TM_(problemDict.at(RULE_DISTUPGRADE), 1), solvid2str(pool, source).c_str(),
                               pool_id2solvable(pool, source)->repo->name);
        case SOLVER_RULE_INFARCH:
            return tfm::format(TM_(problemDict.at(RULE_DISTUPGRADE), 1), solvid2str(pool, source).c_str(),
                               pool_id2solvable(pool, source)->repo->name);
        case SOLVER_RULE_UPDATE:
            return std::string(TM_(problemDict.at(RULE_UPDATE), 1)) + solvid2str(pool, source);
        case SOLVER_RULE_JOB:
            return std::string(TM_(problemDict.at(RULE_JOB), 1));
        case SOLVER_RULE_JOB_UNSUPPORTED:
            return std::string(TM_(problemDict.at(RULE_JOB_UNSUPPORTED), 1));
        case SOLVER_RULE_JOB_NOTHING_PROVIDES_DEP:
            return std::string(TM_(problemDict.at(RULE_JOB_NOTHING_PROVIDES_DEP), 1)) + pool_dep2str(pool, dep);
        case SOLVER_RULE_JOB_UNKNOWN_PACKAGE:
            return tfm::format(TM_(problemDict.at(RULE_JOB_UNKNOWN_PACKAGE), 1), pool_dep2str(pool, dep));
        case SOLVER_RULE_JOB_PROVIDED_BY_SYSTEM:
            return std::string(pool_dep2str(pool, dep)) + TM_(problemDict.at(RULE_JOB_PROVIDED_BY_SYSTEM), 1);
        case SOLVER_RULE_PKG:
            return std::string(TM_(problemDict.at(RULE_PKG), 1));
        case SOLVER_RULE_BEST:
            if (source > 0)
                return std::string(TM_(problemDict.at(RULE_BEST_1), 1)) + solvid2str(pool, source);
            return std::string(TM_(problemDict.at(RULE_BEST_2), 1));
        case SOLVER_RULE_PKG_NOT_INSTALLABLE:
            ss = pool->solvables + source;
            if (pool_disabled_solvable(pool, ss)) {
                if (modularExclude && modularExclude->has(source)) {
                    return tfm::format(TM_(problemDict.at(RULE_PKG_NOT_INSTALLABLE_1), 1),
                                       solvid2str(pool, source).c_str(), pool_id2solvable(pool, source)->repo->name);
                } else {
                    return tfm::format(TM_(problemDict.at(RULE_PKG_NOT_INSTALLABLE_4), 1),
                                       solvid2str(pool, source).c_str(), pool_id2solvable(pool, source)->repo->name);
                }
            }
            if (ss->arch && ss->arch != ARCH_SRC && ss->arch != ARCH_NOSRC &&
                pool->id2arch && (ss->arch > pool->lastarch || !pool->id2arch[ss->arch]))
                return tfm::format(TM_(problemDict.at(RULE_PKG_NOT_INSTALLABLE_2), 1), solvid2str(pool, source).c_str(),
                                   pool_id2solvable(pool, source)->repo->name);
            return tfm::format(TM_(problemDict.at(RULE_PKG_NOT_INSTALLABLE_3), 1), solvid2str(pool, source).c_str(),
                               pool_id2solvable(pool, source)->repo->name);
        case SOLVER_RULE_PKG_NOTHING_PROVIDES_DEP:
            return tfm::format(TM_(problemDict.at(RULE_PKG_NOTHING_PROVIDES_DEP), 1), pool_dep2str(pool, dep),
                               solvid2str(pool, source).c_str(), pool_id2solvable(pool, source)->repo->name);
        case SOLVER_RULE_PKG_SAME_NAME:
            return tfm::format(TM_(problemDict.at(RULE_PKG_SAME_NAME), 1), solvid2str(pool, source).c_str(),
                               pool_id2solvable(pool, source)->repo->name, solvid2str(pool, target).c_str(),
                               pool_id2solvable(pool, target)->repo->name);
        case SOLVER_RULE_PKG_CONFLICTS:
            return tfm::format(TM_(problemDict.at(RULE_PKG_CONFLICTS), 1), solvid2str(pool, source).c_str(),
                               pool_id2solvable(pool, source)->repo->name, pool_dep2str(pool, dep),
                               solvid2str(pool, target).c_str(), pool_id2solvable(pool, target)->repo->name);
        case SOLVER_RULE_PKG_OBSOLETES:
            return tfm::format(TM_(problemDict.at(RULE_PKG_OBSOLETES), 1), solvid2str(pool, source).c_str(),
                               pool_id2solvable(pool, source)->repo->name, pool_dep2str(pool, dep),
                               solvid2str(pool, target).c_str(), pool_id2solvable(pool, target)->repo->name);
        case SOLVER_RULE_PKG_INSTALLED_OBSOLETES:
            return tfm::format(TM_(problemDict.at(RULE_PKG_INSTALLED_OBSOLETES), 1),
                               solvid2str(pool, source).c_str(), pool_dep2str(pool, dep),
                               solvid2str(pool, target).c_str(), pool_id2solvable(pool, target)->repo->name);
        case SOLVER_RULE_PKG_IMPLICIT_OBSOLETES:
            return tfm::format(TM_(problemDict.at(RULE_PKG_IMPLICIT_OBSOLETES), 1),
                               solvid2str(pool, source).c_str(), pool_dep2str(pool, dep),
                               pool_id2solvable(pool, source)->repo->name, solvid2str(pool, target).c_str(),
                               pool_id2solvable(pool, target)->repo->name);
        case SOLVER_RULE_PKG_REQUIRES:
            return tfm::format(TM_(problemDict.at(RULE_PKG_REQUIRES), 1), solvid2str(pool, source).c_str(),
                               pool_id2solvable(pool, source)->repo->name, pool_dep2str(pool, dep));
        case SOLVER_RULE_PKG_SELF_CONFLICT:
            return tfm::format(TM_(problemDict.at(RULE_PKG_SELF_CONFLICT), 1), solvid2str(pool, source).c_str(),
                               pool_id2solvable(pool, source)->repo->name, pool_dep2str(pool, dep));
        case SOLVER_RULE_YUMOBS:
            return tfm::format(TM_(problemDict.at(RULE_YUMOBS), 1), solvid2str(pool, source).c_str(),
                               pool_id2solvable(pool, source)->repo->name, solvid2str(pool, target).c_str(),
                               pool_id2solvable(pool, target)->repo->name, pool_dep2str(pool, dep));
        default:
            return solver_problemruleinfo2str(solv, type, source, target, dep);
    }
}

static void
packageToJob(DnfPackage * package, Queue * job, int solver_action)
{
    IdQueue pkgs;

    Pool *pool = dnf_package_get_pool(package);
    DnfSack *sack = dnf_package_get_sack(package);

    dnf_sack_recompute_considered(sack);
    dnf_sack_make_provides_ready(sack);

    pkgs.pushBack(dnf_package_get_id(package));

    Id what = pool_queuetowhatprovides(pool, pkgs.getQueue());
    queue_push2(job, SOLVER_SOLVABLE_ONE_OF|SOLVER_SETARCH|SOLVER_SETEVR|solver_action, what);
}

static int
jobHas(Queue *job, Id what, Id id)
{
    for (int i = 0; i < job->count; i += 2)
        if (job->elements[i] == what && job->elements[i + 1] == id)
            return 1;
    return 0;
}

static int
filterArchToJob(DnfSack *sack, const Filter *f, Queue *job)
{
    if (!f)
        return 0;

    auto matches = f->getMatches();

    if (f->getCmpType() != HY_EQ) {
        return INCORECT_COMPARISON_TYPE;
    }
    if (matches.size() != 1) {
        return MULTIPLE_MATCH_OBJECTS;
    }
    Pool *pool = dnf_sack_get_pool(sack);
    const char *arch = matches[0].str;
    Id archid = str2archid(pool, arch);

    if (archid == 0)
        return NO_MATCH;
    for (int i = 0; i < job->count; i += 2) {
        Id dep;
        assert((job->elements[i] & SOLVER_SELECTMASK) == SOLVER_SOLVABLE_NAME);
        dep = pool_rel2id(pool, job->elements[i + 1],
                          archid, REL_ARCH, 1);
        job->elements[i] |= SOLVER_SETARCH;
        job->elements[i + 1] = dep;
    }
    return 0;
}

static int
filterEvrToJob(DnfSack *sack, const Filter *f, Queue *job)
{
    if (!f)
        return 0;
    auto matches = f->getMatches();

    if (f->getCmpType() != HY_EQ) {
        return INCORECT_COMPARISON_TYPE;
    }
    if (matches.size() != 1) {
        return MULTIPLE_MATCH_OBJECTS;
    }

    Pool *pool = dnf_sack_get_pool(sack);
    Id evr = pool_str2id(pool, matches[0].str, 1);
    Id constr = f->getKeyname() == HY_PKG_VERSION ? SOLVER_SETEV : SOLVER_SETEVR;
    for (int i = 0; i < job->count; i += 2) {
        Id dep;
        assert((job->elements[i] & SOLVER_SELECTMASK) == SOLVER_SOLVABLE_NAME);
        dep = pool_rel2id(pool, job->elements[i + 1],
                          evr, REL_EQ, 1);
        job->elements[i] |= constr;
        job->elements[i + 1] = dep;
    }
    return 0;
}

static int
filterFileToJob(DnfSack *sack, const Filter *f, Queue *job)
{
    if (!f)
        return 0;

    auto matches = f->getMatches();

    if (matches.size() != 1) {
        return MULTIPLE_MATCH_OBJECTS;
    }

    const char *file = matches[0].str;
    Pool *pool = dnf_sack_get_pool(sack);

    int flags = f->getCmpType() & HY_GLOB ? SELECTION_GLOB : 0;
    if (f->getCmpType() & HY_GLOB)
        flags |= SELECTION_NOCASE;
    if (selection_make(pool, job, file, flags | SELECTION_FILELIST) == 0)
        return NO_MATCH;
    return 0;
}

static int
filterPkgToJob(Id what, Queue *job)
{
    if (!what)
        return 0;
    queue_push2(job, SOLVER_SOLVABLE_ONE_OF|SOLVER_SETARCH|SOLVER_SETEVR, what);
    return 0;
}

static int
filterNameToJob(DnfSack *sack, const Filter *f, Queue *job)
{
    if (!f)
        return 0;
    if (f->getMatches().size() != 1)
        return MULTIPLE_MATCH_OBJECTS;

    Pool *pool = dnf_sack_get_pool(sack);
    const char *name = f->getMatches()[0].str;
    Id id;
    Dataiterator di;

    switch (f->getCmpType()) {
    case HY_EQ:
        id = pool_str2id(pool, name, 0);
        if (id)
            queue_push2(job, SOLVER_SOLVABLE_NAME, id);
        break;
    case HY_GLOB:
        dataiterator_init(&di, pool, 0, 0, SOLVABLE_NAME, name, SEARCH_GLOB);
        while (dataiterator_step(&di)) {
            if (!is_package(pool, pool_id2solvable(pool, di.solvid)))
                continue;
            assert(di.idp);
            id = *di.idp;
            if (jobHas(job, SOLVABLE_NAME, id))
                continue;
            queue_push2(job, SOLVER_SOLVABLE_NAME, id);
        }
        dataiterator_free(&di);
        break;
    default:
        return INCORECT_COMPARISON_TYPE;
    }
    return 0;
}

static int
filterProvidesToJob(DnfSack *sack, const Filter *f, Queue *job)
{
    if (!f)
        return 0;
    auto matches = f->getMatches();
    if (f->getMatches().size() != 1)
        return MULTIPLE_MATCH_OBJECTS;
    const char *name;
    Pool *pool = dnf_sack_get_pool(sack);
    Id id;
    Dataiterator di;

    switch (f->getCmpType()) {
        case HY_EQ:
            id = matches[0].reldep;
            queue_push2(job, SOLVER_SOLVABLE_PROVIDES, id);
            break;
        case HY_GLOB:
            name = matches[0].str;
            dataiterator_init(&di, pool, 0, 0, SOLVABLE_PROVIDES, name, SEARCH_GLOB);
            while (dataiterator_step(&di)) {
                if (is_package(pool, pool_id2solvable(pool, di.solvid)))
                    break;
            }
            assert(di.idp);
            id = *di.idp;
            if (!jobHas(job, SOLVABLE_PROVIDES, id))
                queue_push2(job, SOLVER_SOLVABLE_PROVIDES, id);
            dataiterator_free(&di);
            break;
        default:
            return INCORECT_COMPARISON_TYPE;
    }
    return 0;
}

static int
filterReponameToJob(DnfSack *sack, const Filter *f, Queue *job)
{
    Id i;
    LibsolvRepo *repo;

    if (!f)
        return 0;
    auto matches = f->getMatches();

    if (f->getCmpType() != HY_EQ) {
        return INCORECT_COMPARISON_TYPE;
    }
    if (matches.size() != 1) {
        return MULTIPLE_MATCH_OBJECTS;
    }

    IdQueue repo_sel;
    Pool *pool = dnf_sack_get_pool(sack);
    FOR_REPOS(i, repo)
        if (!strcmp(matches[0].str, repo->name)) {
            repo_sel.pushBack(SOLVER_SOLVABLE_REPO | SOLVER_SETREPO, repo->repoid);
        }

    selection_filter(pool, job, repo_sel.getQueue());

    return 0;
}

/**
 * Build job queue from a Query.
 *
 * Returns an error code
 */
void
sltrToJob(const HySelector sltr, Queue *job, int solver_action)
{
    DnfSack *sack = sltr->getSack();
    int ret = 0;

    int any_opt_filter = sltr->getFilterArch() || sltr->getFilterEvr()
        || sltr->getFilterReponame();
    int any_req_filter = sltr->getFilterName() || sltr->getFilterProvides()
        || sltr->getFilterFile() || sltr->getPkgs();

    IdQueue job_sltr;

    if (!any_req_filter) {
        if (any_opt_filter) {
            // no name or provides or file in the selector is an error
            throw Goal::Error("Ill-formed Selector. No name or"
                "provides or file in the selector.", DNF_ERROR_BAD_SELECTOR);
        }
        goto finish;
    }

    dnf_sack_recompute_considered(sack);
    dnf_sack_make_provides_ready(sack);
    ret = filterPkgToJob(sltr->getPkgs(), job_sltr.getQueue());
    if (ret)
        goto finish;
    ret = filterNameToJob(sack, sltr->getFilterName(), job_sltr.getQueue());
    if (ret)
        goto finish;
    ret = filterFileToJob(sack, sltr->getFilterFile(), job_sltr.getQueue());
    if (ret)
        goto finish;
    ret = filterProvidesToJob(sack, sltr->getFilterProvides(), job_sltr.getQueue());
    if (ret)
        goto finish;
    ret = filterArchToJob(sack, sltr->getFilterArch(), job_sltr.getQueue());
    if (ret)
        goto finish;
    ret = filterEvrToJob(sack, sltr->getFilterEvr(), job_sltr.getQueue());
    if (ret)
        goto finish;
    ret = filterReponameToJob(sack, sltr->getFilterReponame(), job_sltr.getQueue());
    if (ret)
        goto finish;

    for (int i = 0; i < job_sltr.size(); i += 2)
         queue_push2(job, job_sltr[i] | solver_action, job_sltr[i + 1]);

 finish:
    if (ret > 1) {
        throw Goal::Error(TM_(ERROR_DICT[ret], 1), DNF_ERROR_BAD_SELECTOR);
    }
}

#define BLOCK_SIZE 15

struct InstallonliesSortCallback {
    Pool *pool;
    Id running_kernel;
};

static inline void
queue2pset(const IdQueue & queue, PackageSet * pset)
{
    for (int i = 0; i < queue.size(); ++i)
        pset->set(queue[i]);
}

static bool
/**
* @brief return false iff a does not depend on anything from b
*/
can_depend_on(Pool *pool, Solvable *sa, Id b)
{
    IdQueue dep_requires;

    solvable_lookup_idarray(sa, SOLVABLE_REQUIRES, dep_requires.getQueue());
    for (int i = 0; i < dep_requires.size(); ++i) {
        Id req_dep = dep_requires[i];
        Id p, pp;

        FOR_PROVIDES(p, pp, req_dep)
            if (p == b)
                return true;
    }

    return false;
}

static int
sort_packages(const void *ap, const void *bp, void *s_cb)
{
    Id a = *(Id*)ap;
    Id b = *(Id*)bp;
    Pool *pool = ((struct InstallonliesSortCallback*) s_cb)->pool;
    Id kernel = ((struct InstallonliesSortCallback*) s_cb)->running_kernel;
    Solvable *sa = pool_id2solvable(pool, a);
    Solvable *sb = pool_id2solvable(pool, b);

    /* if the names are different sort them differently, particular order does
       not matter as long as it's consistent. */
    int name_diff = sa->name - sb->name;
    if (name_diff)
        return name_diff;

    /* same name, if one is/depends on the running kernel put it last */

    /* move available packages to end of the list */
    if (pool->installed != sa->repo)
        return 1;

    if (pool->installed != sb->repo)
        return -1;

    if (kernel >= 0) {
        if (a == kernel || can_depend_on(pool, sa, kernel))
            return 1;
        if (b == kernel || can_depend_on(pool, sb, kernel))
            return -1;
        // if package has same evr as kernel try them to keep (kernel-devel packages)
        Solvable * kernelSolvable = pool_id2solvable(pool, kernel);
        if (sa->evr == kernelSolvable->evr) {
            return 1;
        }
        if (sb->evr == kernelSolvable->evr) {
            return -1;
        }
    }
    return pool_evrcmp(pool, sa->evr, sb->evr, EVRCMP_COMPARE);
}

static void
same_name_subqueue(Pool *pool, Queue *in, Queue *out)
{
    Id el = queue_pop(in);
    Id name = pool_id2solvable(pool, el)->name;
    queue_empty(out);
    queue_push(out, el);
    while (in->count &&
           pool_id2solvable(pool, in->elements[in->count - 1])->name == name)
        // reverses the order so packages are sorted by descending version
        queue_push(out, queue_pop(in));
}

static std::unique_ptr<PackageSet>
remove_pkgs_with_same_nevra_from_pset(DnfPackageSet* pset, DnfPackageSet* remove_musters,
                                      DnfSack* sack)
{
    std::unique_ptr<PackageSet> final_pset(new PackageSet(sack));
    Id id1 = -1;
    while(true) {
        id1 = pset->next(id1);
        if (id1 == -1)
            break;
        DnfPackage *pkg1 = dnf_package_new(sack, id1);
        Id id2 = -1;
        bool found = false;
        while(true) {
            id2 = remove_musters->next(id2);
            if (id2 == -1)
                break;
            DnfPackage *pkg2 = dnf_package_new(sack, id2);
            if (!dnf_package_cmp(pkg1, pkg2)) {
                found = true;
                g_object_unref(pkg2);
                break;
            }
            g_object_unref(pkg2);
        }
        if (!found) {
            final_pset->set(pkg1);
        }
        g_object_unref(pkg1);
    }
    return final_pset;
}

static int
erase_flags2libsolv(int flags)
{
    int ret = 0;
    if (flags & HY_CLEAN_DEPS)
        ret |= SOLVER_CLEANDEPS;
    return ret;
}

Goal::Goal(const Goal & goal_src) : pImpl(new Impl(*goal_src.pImpl)) {}

Goal::Impl::Impl(const Goal::Impl & goal_src)
: sack(goal_src.sack), exclude_from_weak(goal_src.exclude_from_weak)
{
    queue_init_clone(&staging, const_cast<Queue *>(&goal_src.staging));

    actions = goal_src.actions;
    if (goal_src.protectedPkgs) {
        protectedPkgs.reset(new PackageSet(*goal_src.protectedPkgs.get()));
    }
    if (goal_src.removalOfProtected) {
        removalOfProtected.reset(new PackageSet(*goal_src.removalOfProtected.get()));
    }
}

Goal::Impl::Impl(DnfSack *sack)
: sack(sack), exclude_from_weak(sack)
{
    queue_init(&staging);
}

Goal::Goal(DnfSack *sack) : pImpl(new Impl(sack)) {}

Goal::~Goal() = default;

Goal::Impl::~Impl()
{
    if (trans)
        transaction_free(trans);
    if (solv)
        solver_free(solv);
    queue_free(&staging);
}

DnfGoalActions Goal::getActions() { return pImpl->actions; }

DnfSack * Goal::getSack() { return pImpl->sack; }

int
Goal::getReason(DnfPackage *pkg)
{
    //solver_get_recommendations
    if (!pImpl->solv)
        return HY_REASON_USER;
    Id info;
    const Id pkgID = dnf_package_get_id(pkg);
    int reason = solver_describe_decision(pImpl->solv, pkgID, &info);

    if ((reason == SOLVER_REASON_UNIT_RULE ||
         reason == SOLVER_REASON_RESOLVE_JOB) &&
        (solver_ruleclass(pImpl->solv, info) == SOLVER_RULE_JOB ||
         solver_ruleclass(pImpl->solv, info) == SOLVER_RULE_BEST))
        return HY_REASON_USER;
    if (reason == SOLVER_REASON_CLEANDEPS_ERASE)
        return HY_REASON_CLEAN;
    if (reason == SOLVER_REASON_WEAKDEP)
        return HY_REASON_WEAKDEP;
    IdQueue cleanDepsQueue;
    solver_get_cleandeps(pImpl->solv, cleanDepsQueue.getQueue());
    for (int i = 0; i < cleanDepsQueue.size(); ++i) {
        if (cleanDepsQueue[i] == pkgID) {
            return HY_REASON_CLEAN;
        }
    }
    return HY_REASON_DEP;
}

void
Goal::addProtected(PackageSet & pset)
{
    if (!pImpl->protectedPkgs) {
        pImpl->protectedPkgs.reset(new PackageSet(pset));
    } else {
        map_or(pImpl->protectedPkgs->getMap(), pset.getMap());
    }
}

bool
Goal::get_protect_running_kernel() const noexcept
{
    return pImpl->protect_running_kernel;
}

void
Goal::set_protect_running_kernel(bool value)
{
    pImpl->protect_running_kernel = value;
}

void
Goal::setProtected(const PackageSet & pset)
{
    pImpl->protectedPkgs.reset(new PackageSet(pset));
}

void
Goal::distupgrade()
{
    pImpl->actions = static_cast<DnfGoalActions>(pImpl->actions | DNF_DISTUPGRADE|DNF_ALLOW_DOWNGRADE);
    DnfSack * sack = pImpl->sack;
    Query query(sack);
    query.available();
    Selector selector(sack);
    selector.set(query.runSet());
    sltrToJob(&selector, &pImpl->staging, SOLVER_DISTUPGRADE);
}

void
Goal::distupgrade(DnfPackage *new_pkg)
{
    pImpl->actions = static_cast<DnfGoalActions>(pImpl->actions | DNF_DISTUPGRADE|DNF_ALLOW_DOWNGRADE);
    packageToJob(new_pkg, &pImpl->staging, SOLVER_DISTUPGRADE);
}

void
Goal::distupgrade(HySelector sltr)
{
    pImpl->actions = static_cast<DnfGoalActions>(pImpl->actions | DNF_DISTUPGRADE|DNF_ALLOW_DOWNGRADE);
    sltrToJob(sltr, &pImpl->staging, SOLVER_DISTUPGRADE);
}

void
Goal::erase(DnfPackage *pkg, int flags)
{
    int additional = erase_flags2libsolv(flags);
    pImpl->actions = static_cast<DnfGoalActions>(pImpl->actions | DNF_ERASE);
    queue_push2(&pImpl->staging, SOLVER_SOLVABLE|SOLVER_ERASE|additional, dnf_package_get_id(pkg));
}

void
Goal::erase(HySelector sltr, int flags)
{
    int additional = erase_flags2libsolv(flags);
    pImpl->actions = static_cast<DnfGoalActions>(pImpl->actions | DNF_ERASE);
    sltrToJob(sltr, &pImpl->staging, SOLVER_ERASE|additional);
}

void
Goal::install(DnfPackage *new_pkg, bool optional)
{
    int solverActions = SOLVER_INSTALL;
    if (optional) {
        solverActions |= SOLVER_WEAK;
    }
    pImpl->actions = static_cast<DnfGoalActions>(pImpl->actions | DNF_INSTALL|DNF_ALLOW_DOWNGRADE);
    packageToJob(new_pkg, &pImpl->staging, solverActions);
}

void
Goal::lock(DnfPackage *pkg)
{
    queue_push2(&pImpl->staging, SOLVER_SOLVABLE|SOLVER_LOCK, dnf_package_get_id(pkg));
}

void
Goal::favor(DnfPackage *pkg)
{
    queue_push2(&pImpl->staging, SOLVER_SOLVABLE|SOLVER_FAVOR, dnf_package_get_id(pkg));
}

void
Goal::add_exclude_from_weak(const DnfPackageSet & pset)
{
    pImpl->exclude_from_weak += pset;
}

void
Goal::add_exclude_from_weak(DnfPackage *pkg)
{
    // ensure that the map has a corrent size before set to prevent memory corruption
    map_grow(pImpl->exclude_from_weak.getMap(), dnf_sack_get_pool(pImpl->sack)->nsolvables);
    pImpl->exclude_from_weak.set(pkg);
}

void
Goal::reset_exclude_from_weak()
{
    pImpl->exclude_from_weak.clear();
}

void
Goal::exclude_from_weak_autodetect()
{
    Query installed_query(pImpl->sack, Query::ExcludeFlags::IGNORE_EXCLUDES);
    installed_query.installed();
    if (installed_query.empty()) {
        return;
    }
    Query base_query(pImpl->sack);
    base_query.apply();
    auto * installed_pset = installed_query.getResultPset();
    Id installed_id = -1;

    std::vector<const char *> installed_names;
    installed_names.reserve(installed_pset->size() + 1);

    // Iterate over installed packages to detect unmet weak deps
    while ((installed_id = installed_pset->next(installed_id)) != -1) {
        g_autoptr(DnfPackage) pkg = dnf_package_new(pImpl->sack, installed_id);
        installed_names.push_back(dnf_package_get_name(pkg));
        std::unique_ptr<libdnf::DependencyContainer> recommends(dnf_package_get_recommends(pkg));
        for (int i = 0; i < recommends->count(); ++i) {
            std::unique_ptr<libdnf::Dependency> dep(recommends->getPtr(i));
            const char * dep_string = dep->toString();
            if (dep_string[0] == '(') {
                continue;
            }
            Query query(base_query);
            const char * version = dep->getVersion();
            //  There can be installed provider in different version or upgraded packed can recommend a different version
            //  Ignore version and search only by reldep name
            if (version && strlen(version) > 0) {
                query.addFilter(HY_PKG_PROVIDES, HY_EQ, dep->getName());
            } else {
                query.addFilter(HY_PKG_PROVIDES, dep.get());
            }
            // No providers of recommend => continue
            if (query.empty()) {
                continue;
            }
            Query test_installed(query);
            test_installed.installed();
            // when there is not installed any provider of recommend, exclude it
            if (test_installed.empty()) {
                add_exclude_from_weak(*query.getResultPset());
            }
        }
    }

    // Investigate supplements of only available packages with a different name to installed packages
    installed_names.push_back(nullptr);
    base_query.addFilter(HY_PKG_NAME, HY_NEQ, installed_names.data());
    auto * available_pset = base_query.getResultPset();
    *available_pset -= *installed_pset;
    Id available_id = -1;
    while ((available_id = available_pset->next(available_id)) != -1) {
        g_autoptr(DnfPackage) pkg = dnf_package_new(pImpl->sack, available_id);
        std::unique_ptr<libdnf::DependencyContainer> supplements(dnf_package_get_supplements(pkg));
        if (supplements->count() == 0) {
            continue;
        }
        libdnf::DependencyContainer supplements_without_rich(getSack());
        for (int i = 0; i < supplements->count(); ++i) {
            std::unique_ptr<libdnf::Dependency> dep(supplements->getPtr(i));
            const char * dep_string = dep->toString();
            if (dep_string[0] == '(') {
                continue;
            }
            supplements_without_rich.add(dep.get());
        }
        if (supplements_without_rich.count() == 0) {
            continue;
        }
        Query query(installed_query);
        query.addFilter(HY_PKG_PROVIDES, &supplements_without_rich);
        // When supplemented package already installed, exclude_from_weak available package
        if (!query.empty()) {
            add_exclude_from_weak(pkg);
        }
    }
}

void
Goal::disfavor(DnfPackage *pkg)
{
    queue_push2(&pImpl->staging, SOLVER_SOLVABLE|SOLVER_DISFAVOR, dnf_package_get_id(pkg));
}

void
Goal::install(HySelector sltr, bool optional)
{
    int solverActions = SOLVER_INSTALL;
    if (optional) {
        solverActions |= SOLVER_WEAK;
    }
    pImpl->actions = static_cast<DnfGoalActions>(pImpl->actions | DNF_INSTALL|DNF_ALLOW_DOWNGRADE);
    sltrToJob(sltr, &pImpl->staging, solverActions);
}

void
Goal::upgrade()
{
    pImpl->actions = static_cast<DnfGoalActions>(pImpl->actions | DNF_UPGRADE_ALL);
    queue_push2(&pImpl->staging, SOLVER_UPDATE|SOLVER_SOLVABLE_ALL, 0);
}

void
Goal::upgrade(DnfPackage *new_pkg)
{
    pImpl->actions = static_cast<DnfGoalActions>(pImpl->actions | DNF_UPGRADE);
    packageToJob(new_pkg, &pImpl->staging, SOLVER_UPDATE);
}

void
Goal::upgrade(HySelector sltr)
{
    pImpl->actions = static_cast<DnfGoalActions>(pImpl->actions | DNF_UPGRADE);
    auto flags = SOLVER_UPDATE;
    if (sltr->getPkgs()) {
        flags |= SOLVER_TARGETED;
    }
    sltrToJob(sltr, &pImpl->staging, flags);
}

void
Goal::userInstalled(DnfPackage *pkg)
{
    queue_push2(&pImpl->staging, SOLVER_SOLVABLE|SOLVER_USERINSTALLED, dnf_package_get_id(pkg));
}

void
Goal::userInstalled(PackageSet & pset)
{
    Id id = -1;
    while (true) {
        id = pset.next(id);
        if (id == -1)
            break;
        queue_push2(&pImpl->staging, SOLVER_SOLVABLE|SOLVER_USERINSTALLED, id);
    }
}

bool
Goal::hasActions(DnfGoalActions action)
{
    return pImpl->actions & action;
}

int
Goal::jobLength()
{
    return (&pImpl->staging)->count / 2;
}

bool
Goal::run(DnfGoalActions flags)
{
    auto job = pImpl->constructJob(flags);
    pImpl->actions = static_cast<DnfGoalActions>(pImpl->actions | flags);
    int ret = pImpl->solve(job->getQueue(), flags);
    return ret;
}

int
Goal::countProblems()
{
    return pImpl->countProblems();
}

/**
 * Reports packages that has a conflict
 *
 * available - if available it returns set with available packages with conflicts
 * available - if package installed it also excludes available packages with same NEVRA
 *
 * Returns DnfPackageSet with all packages that have a conflict.
 */
std::unique_ptr<PackageSet>
Goal::listConflictPkgs(DnfPackageState pkg_type)
{
    DnfSack * sack = pImpl->sack;
    Pool * pool = dnf_sack_get_pool(sack);
    std::unique_ptr<PackageSet> pset(new PackageSet(sack));
    PackageSet temporary_pset(sack);

    int countProblemsValue = pImpl->countProblems();
    for (int i = 0; i < countProblemsValue; i++) {
        auto conflict = pImpl->conflictPkgs(i);
        for (int j = 0; j < conflict->size(); j++) {
            Id id = (*conflict)[j];
            Solvable *s = pool_id2solvable(pool, id);
            bool installed = pool->installed == s->repo;
            if (pkg_type ==  DNF_PACKAGE_STATE_AVAILABLE && installed) {
                temporary_pset.set(id);
                continue;
            }
            if (pkg_type ==  DNF_PACKAGE_STATE_INSTALLED && !installed)
                continue;
            pset->set(id);
        }
    }
    if (!temporary_pset.size()) {
        return pset;
    }

    return remove_pkgs_with_same_nevra_from_pset(pset.get(), &temporary_pset, sack);
}

/**
 * Reports all packages that have broken dependency
 * available - if available returns only available packages with broken dependencies
 * available - if package installed it also excludes available packages with same NEVRA
 * Returns DnfPackageSet with all packages with broken dependency
 */
std::unique_ptr<PackageSet>
Goal::listBrokenDependencyPkgs(DnfPackageState pkg_type)
{
    return pImpl->brokenDependencyAllPkgs(pkg_type);
}

std::vector<std::vector<std::string>> Goal::describeAllProblemRules(bool pkgs)
{
    std::vector<std::vector<std::string>> output;
    int count_problems = countProblems();
    for (int i = 0; i < count_problems; i++) {
        auto problemList = describeProblemRules(i, pkgs);
        if (problemList.empty()) {
            continue;
        }
        bool unique = true;
        for (auto & problemsSaved: output) {
            if (problemList.size() != problemsSaved.size()) {
                continue;
            }
            bool presentElement = false;
            for (auto & problem: problemList) {
                presentElement = false;
                for (auto & problemSaved: problemsSaved) {
                    if (problemSaved == problem) {
                        presentElement = true;
                        break;
                    }
                }
                if (!presentElement) {
                    break;
                }
            }
            if (presentElement) {
                unique = false;
            }
        }
        if (unique) {
            output.push_back(problemList);
        }
    }
    return output;
}

std::vector<std::string>
Goal::describeProblemRules(unsigned i, bool pkgs)
{
    std::vector<std::string> output;
    /* internal error */
    if (i >= (unsigned) pImpl->countProblems())
        return output;
    // problem is not in libsolv - removal of protected packages
    auto problem = pImpl->describeProtectedRemoval();
    if (!problem.empty()) {
        output.push_back(std::move(problem));
        return output;
    }
    auto solv = pImpl->solv;

    Id rid, source, target, dep;
    SolverRuleinfo type;
    int j;
    bool unique;

    if (i >= solver_problem_count(solv))
        return output;

    IdQueue pq;
    IdQueue rq;
    // this libsolv interface indexes from 1 (we do from 0), so:
    solver_findallproblemrules(solv, i+1, pq.getQueue());
    std::unique_ptr<libdnf::PackageSet> modularExcludes(dnf_sack_get_module_excludes(pImpl->sack));
    for (j = 0; j < pq.size(); j++) {
        rid = pq[j];
        if (solver_allruleinfos(solv, rid, rq.getQueue())) {
            for (int ir = 0; ir < rq.size(); ir+=4) {
                type = static_cast<SolverRuleinfo>(rq[ir]);
                source = rq[ir + 1];
                target = rq[ir + 2];
                dep = rq[ir + 3];
                auto problem_str = libdnf_problemruleinfo2str(modularExcludes.get(), solv, type,
                                                              source, target, dep, pkgs);
                unique = true;
                for (auto & item: output) {
                    if (problem_str == item) {
                        unique = false;
                        break;
                    }
                }
                if (unique) {
                    output.push_back(problem_str);
                }
            }
        }
    }
    return output;
}

/**
 * Write all the solving decisions to the hawkey logfile.
 */
int
Goal::logDecisions()
{
    if (!pImpl->solv)
        return 1;
    solver_printdecisionq(pImpl->solv, SOLV_DEBUG_RESULT);
    return 0;
}

/**
 * hy_goal_write_debugdata:
 * @goal: A #HyGoal
 * @dir: The directory to write to
 * @error: A #GError, or %NULL
 *
 * Writes details about the testcase to a directory.
 *
 * Returns: %false if an error was set
 *
 * Since: 0.7.0
 */
void
Goal::writeDebugdata(const char *dir)
{
    Solver *solv = pImpl->solv;
    if (!solv) {
        throw Goal::Error(_("no solver set"), DNF_ERROR_INTERNAL_ERROR);
    }
    int flags = TESTCASE_RESULT_TRANSACTION | TESTCASE_RESULT_PROBLEMS;
    g_autofree char *absdir = abspath(dir);
    if (!absdir) {
        std::string msg = tfm::format(_("failed to make %s absolute"), dir);
        throw Goal::Error(msg, DNF_ERROR_FILE_INVALID);
    }
    makeDirPath(dir);
    g_debug("writing solver debugdata to %s", absdir);
    int ret = testcase_write(solv, absdir, flags, NULL, NULL);
    if (!ret) {
        std::string msg = tfm::format(_("failed writing debugdata to %1$s: %2$s"),
                                      absdir, strerror(errno));
        throw Goal::Error(msg, DNF_ERROR_FILE_INVALID);
    }
}

PackageSet
Goal::Impl::listResults(Id type_filter1, Id type_filter2)
{
    /* no transaction */
    if (!trans) {
        if (!solv) {
            throw Goal::Error(_("no solv in the goal"), DNF_ERROR_INTERNAL_ERROR);
        } else if (removalOfProtected && removalOfProtected->size()) {
            throw Goal::Error(_("no solution, cannot remove protected package"),
                                          DNF_ERROR_REMOVAL_OF_PROTECTED_PKG);
        }
        throw Goal::Error(_("no solution possible"), DNF_ERROR_NO_SOLUTION);
    }

    PackageSet plist(sack);
    const int common_mode = SOLVER_TRANSACTION_SHOW_OBSOLETES |
        SOLVER_TRANSACTION_CHANGE_IS_REINSTALL;

    for (int i = 0; i < trans->steps.count; ++i) {
        Id p = trans->steps.elements[i];
        Id type;

        switch (type_filter1) {
        case SOLVER_TRANSACTION_OBSOLETED:
            type =  transaction_type(trans, p, common_mode);
            break;
        default:
            type  = transaction_type(trans, p, common_mode |
                                     SOLVER_TRANSACTION_SHOW_ACTIVE|
                                     SOLVER_TRANSACTION_SHOW_ALL);
            break;
        }

        if (type == type_filter1 || (type_filter2 && type == type_filter2))
            plist.set(p);
    }
    return plist;
}

PackageSet
Goal::listErasures()
{
    return pImpl->listResults(SOLVER_TRANSACTION_ERASE, 0);
}

PackageSet
Goal::listInstalls()
{
    return pImpl->listResults(SOLVER_TRANSACTION_INSTALL, SOLVER_TRANSACTION_OBSOLETES);
}

PackageSet
Goal::listObsoleted()
{
    return pImpl->listResults(SOLVER_TRANSACTION_OBSOLETED, 0);
}

PackageSet
Goal::listReinstalls()
{
    return pImpl->listResults(SOLVER_TRANSACTION_REINSTALL, 0);
}

PackageSet
Goal::listUnneeded()
{
    PackageSet pset(pImpl->sack);
    IdQueue queue;
    Solver *solv = pImpl->solv;

    solver_get_unneeded(solv, queue.getQueue(), 0);
    queue2pset(queue, &pset);
    return pset;
}

PackageSet
Goal::listSuggested()
{
    PackageSet pset(pImpl->sack);
    IdQueue queue;
    Solver *solv = pImpl->solv;

    solver_get_recommendations(solv, NULL, queue.getQueue(), 0);
    queue2pset(queue, &pset);
    return pset;
}

PackageSet
Goal::listUpgrades()
{
    return pImpl->listResults(SOLVER_TRANSACTION_UPGRADE, 0);
}

PackageSet
Goal::listDowngrades()
{
    return pImpl->listResults(SOLVER_TRANSACTION_DOWNGRADE, 0);
}

PackageSet
Goal::listObsoletedByPackage(DnfPackage *pkg)
{
    auto trans = pImpl->trans;
    IdQueue obsoletes;
    PackageSet pset(pImpl->sack);

    assert(trans);

    transaction_all_obs_pkgs(trans, dnf_package_get_id(pkg), obsoletes.getQueue());
    queue2pset(obsoletes, &pset);

    return pset;
}

static std::string string_join(const std::vector<std::string> & src, const std::string & delim)
{
    if (src.empty()) {
        return {};
    }
    std::string output(*src.begin());
    for (auto iter = std::next(src.begin()); iter != src.end(); ++iter) {
        output.append(delim);
        output.append(*iter);
    }
    return output;
}

std::string
Goal::formatAllProblemRules(const std::vector<std::vector<std::string>> & problems)
{
    if (problems.empty()) {
        return {};
    }
    bool single_problems = problems.size() == 1;
    std::string output;

    if (single_problems) {
        output.append(_("Problem: "));
        output.append(string_join(*problems.begin(), "\n  - "));
        return output;
    }

    const char * problem_prefix = _("Problem %d: ");

    output.append(tfm::format(problem_prefix, 1));
    output.append(string_join(*problems.begin(), "\n  - "));

    int index = 2;
    for (auto iter = std::next(problems.begin()); iter != problems.end(); ++iter) {
        output.append("\n ");
        output.append(tfm::format(problem_prefix, index));
        output.append(string_join(*iter, "\n  - "));
        ++index;
    }
    return output;
}

void
Goal::Impl::allowUninstallAllButProtected(Queue *job, DnfGoalActions flags)
{
    Pool *pool = dnf_sack_get_pool(sack);

    if (!protectedPkgs) {
        protectedPkgs.reset(new PackageSet(sack));
    } else
        map_grow(protectedPkgs->getMap(), pool->nsolvables);

    Id protected_kernel = protectedRunningKernel();

    if (DNF_ALLOW_UNINSTALL & flags)
        for (Id id = 1; id < pool->nsolvables; ++id) {
            Solvable *s = pool_id2solvable(pool, id);
            if (pool->installed == s->repo && !protectedPkgs->has(id) &&
                id != protected_kernel &&
                (!pool->considered || MAPTST(pool->considered, id))) {
                queue_push2(job, SOLVER_ALLOWUNINSTALL|SOLVER_SOLVABLE, id);
            }
        }
}

std::unique_ptr<IdQueue>
Goal::Impl::constructJob(DnfGoalActions flags)
{
    std::unique_ptr<IdQueue> job(new IdQueue(staging));
    auto elements = job->data();
    /* apply forcebest */
    if (flags & DNF_FORCE_BEST)
        for (int i = 0; i < job->size(); i += 2) {
            elements[i] |= SOLVER_FORCEBEST;
    }

    // Add weak excludes to the job
    Id id = -1;
    while ((id = exclude_from_weak.next(id)) != -1) {
        job->pushBack(SOLVER_SOLVABLE|SOLVER_EXCLUDEFROMWEAK, id);
    }

    /* turn off implicit obsoletes for installonly packages */
    for (int i = 0; i < (int) dnf_sack_get_installonly(sack)->count; i++)
        job->pushBack(SOLVER_MULTIVERSION|SOLVER_SOLVABLE_PROVIDES,
            dnf_sack_get_installonly(sack)->elements[i]);

    allowUninstallAllButProtected(job->getQueue(), flags);

    if (flags & DNF_VERIFY)
        job->pushBack(SOLVER_VERIFY|SOLVER_SOLVABLE_ALL, 0);

    return job;
}

Solver *
Goal::Impl::initSolver()
{
    Pool *pool = dnf_sack_get_pool(sack);
    Solver *solvNew = solver_create(pool);

    if (solv)
        solver_free(solv);
    solv = solvNew;

    /* vendor locking */
    int vendor = dnf_sack_get_allow_vendor_change(sack) ? 1 : 0;
    solver_set_flag(solv, SOLVER_FLAG_ALLOW_VENDORCHANGE, vendor);
    solver_set_flag(solv, SOLVER_FLAG_DUP_ALLOW_VENDORCHANGE, vendor);

    /* don't erase packages that are no longer in repo during distupgrade */
    solver_set_flag(solv, SOLVER_FLAG_KEEP_ORPHANS, 1);
    /* no arch change for forcebest */
    solver_set_flag(solv, SOLVER_FLAG_BEST_OBEY_POLICY, 1);
    /* support package splits via obsoletes */
    solver_set_flag(solv, SOLVER_FLAG_YUM_OBSOLETES, 1);

#if defined(LIBSOLV_FLAG_URPMREORDER)
    /* support urpm-like solution reordering */
    solver_set_flag(solv, SOLVER_FLAG_URPM_REORDER, 1);
#endif

    return solv;
}

int
Goal::Impl::limitInstallonlyPackages(Solver *solv, Queue *job)
{
    if (!dnf_sack_get_installonly_limit(sack))
        return 0;

    Queue *onlies = dnf_sack_get_installonly(sack);
    Pool *pool = dnf_sack_get_pool(sack);
    int reresolve = 0;

    for (int i = 0; i < onlies->count; ++i) {
        Id p, pp;
        IdQueue q, installing;

        FOR_PKG_PROVIDES(p, pp, onlies->elements[i])
            if (solver_get_decisionlevel(solv, p) > 0)
                q.pushBack(p);
        if (q.size() <= (int) dnf_sack_get_installonly_limit(sack)) {
            continue;
        }
        for (int k = 0; k < q.size(); ++k) {
            Id id  = q[k];
            Solvable *s = pool_id2solvable(pool, id);
            if (pool->installed != s->repo) {
                installing.pushBack(id);
                break;
            }
        }
        if (!installing.size()) {
            continue;
        }

        struct InstallonliesSortCallback s_cb = {pool, dnf_sack_running_kernel(sack)};
        solv_sort(q.data(), q.size(), sizeof(q[0]), sort_packages, &s_cb);
        IdQueue same_names;
        while (q.size() > 0) {
            same_name_subqueue(pool, q.getQueue(), same_names.getQueue());
            if (same_names.size() <= (int) dnf_sack_get_installonly_limit(sack))
                continue;
            reresolve = 1;
            for (int j = 0; j < same_names.size(); ++j) {
                Id id  = same_names[j];
                Id action = SOLVER_ERASE;
                if (j < (int) dnf_sack_get_installonly_limit(sack))
                    action = SOLVER_INSTALL;
                queue_push2(job, action | SOLVER_SOLVABLE, id);
            }
        }
    }
    return reresolve;
}

bool
Goal::Impl::solve(Queue *job, DnfGoalActions flags)
{
    /* apply the excludes */
    dnf_sack_recompute_considered(sack);

    dnf_sack_make_provides_ready(sack);
    if (trans) {
        transaction_free(trans);
        trans = NULL;
    }

    Solver *solv = initSolver();

    /* Removal of SOLVER_WEAK to allow report errors*/
    if (DNF_IGNORE_WEAK & flags) {
        for (int i = 0; i < job->count; i += 2) {
            job->elements[i] &= ~SOLVER_WEAK;
        }
    }

    if (DNF_IGNORE_WEAK_DEPS & flags)
        solver_set_flag(solv, SOLVER_FLAG_IGNORE_RECOMMENDED, 1);

    if (DNF_ALLOW_DOWNGRADE & actions)
        solver_set_flag(solv, SOLVER_FLAG_ALLOW_DOWNGRADE, 1);

    if (solver_solve(solv, job))
        return true;
    // either allow solutions callback or installonlies, both at the same time
    // are not supported
    if (limitInstallonlyPackages(solv, job)) {
        // allow erasing non-installonly packages that depend on a kernel about
        // to be erased
        allowUninstallAllButProtected(job, DNF_ALLOW_UNINSTALL);
        if (solver_solve(solv, job))
            return true;
    }
    trans = solver_create_transaction(solv);

    if (protectedInRemovals())
        return true;

    return false;
}

/**
 * Reports packages that has a conflict
 *
 * Returns Queue with Ids of packages with conflict
 */
std::unique_ptr<IdQueue>
Goal::Impl::conflictPkgs(unsigned i)
{
    SolverRuleinfo type;
    Id rid, source, target, dep;
    std::unique_ptr<IdQueue> conflict(new IdQueue);
    if (i >= solver_problem_count(solv))
        return conflict;

    IdQueue pq;
    // this libsolv interface indexes from 1 (we do from 0), so:
    solver_findallproblemrules(solv, i+1, pq.getQueue());
    for ( int j = 0; j < pq.size(); j++) {
        rid = pq[j];
        type = solver_ruleinfo(solv, rid, &source, &target, &dep);
        if (type == SOLVER_RULE_PKG_CONFLICTS)
            conflict->pushBack(source, target);
        else if (type == SOLVER_RULE_PKG_SELF_CONFLICT)
            conflict->pushBack(source);
        else if (type == SOLVER_RULE_PKG_SAME_NAME)
            conflict->pushBack(source, target);
    }
    return conflict;
}

int
Goal::Impl::countProblems()
{
    assert(solv);
    size_t protectedSize = removalOfProtected ? removalOfProtected->size() : 0;
    return solver_problem_count(solv) + MIN(1, protectedSize);
}

std::unique_ptr<PackageSet>
Goal::Impl::brokenDependencyAllPkgs(DnfPackageState pkg_type)
{
    Pool * pool = dnf_sack_get_pool(sack);

    std::unique_ptr<PackageSet> pset(new PackageSet(sack));
    PackageSet temporary_pset(sack);

    int countProblemsValue = countProblems();
    for (int i = 0; i < countProblemsValue; i++) {
        auto broken_dependency = brokenDependencyPkgs(i);
        for (int j = 0; j < broken_dependency->size(); j++) {
            Id id = (*broken_dependency)[j];
            Solvable *s = pool_id2solvable(pool, id);
            bool installed = pool->installed == s->repo;
            if (pkg_type ==  DNF_PACKAGE_STATE_AVAILABLE && installed) {
                temporary_pset.set(id);
                continue;
            }
            if (pkg_type ==  DNF_PACKAGE_STATE_INSTALLED && !installed)
                continue;
            pset->set(id);
        }
    }
    if (!temporary_pset.size()) {
        return pset;
    }
    return remove_pkgs_with_same_nevra_from_pset(pset.get(), &temporary_pset, sack);
}

/**
 * Reports packages that have broken dependency
 *
 * Returns Queue with Ids of packages with broken dependency
 */
std::unique_ptr<IdQueue>
Goal::Impl::brokenDependencyPkgs(unsigned i)
{
    SolverRuleinfo type;
    Id rid, source, target, dep;

    auto broken_dependency = std::unique_ptr<IdQueue>(new IdQueue);
    if (i >= solver_problem_count(solv))
        return broken_dependency;
    IdQueue pq;
    // this libsolv interface indexes from 1 (we do from 0), so:
    solver_findallproblemrules(solv, i+1, pq.getQueue());
    for (int j = 0; j < pq.size(); j++) {
        rid = pq[j];
        type = solver_ruleinfo(solv, rid, &source, &target, &dep);
        if (type == SOLVER_RULE_PKG_NOTHING_PROVIDES_DEP)
            broken_dependency->pushBack(source);
        else if (type == SOLVER_RULE_PKG_REQUIRES)
            broken_dependency->pushBack(source);
    }
    return broken_dependency;
}

Id
Goal::Impl::protectedRunningKernel()
{
    return protect_running_kernel ? dnf_sack_running_kernel(sack) : 0;
}

bool
Goal::Impl::protectedInRemovals()
{
    guint i = 0;
    bool ret = false;
    if ((!protectedPkgs || !protectedPkgs->size()) && !protect_running_kernel)
        return false;
    auto pkgRemoveList = listResults(SOLVER_TRANSACTION_ERASE, 0);
    auto pkgObsoleteList = listResults(SOLVER_TRANSACTION_OBSOLETED, 0);
    map_or(pkgRemoveList.getMap(), pkgObsoleteList.getMap());

    removalOfProtected.reset(new PackageSet(pkgRemoveList));
    Id id = -1;
    Id protected_kernel = protectedRunningKernel();
    while(true) {
        id = removalOfProtected->next(id);
        if (id == -1)
            break;

        if (protectedPkgs->has(id) || id == protected_kernel) {
            ret = true;
            i++;
        } else {
            removalOfProtected->remove(id);
        }
    }
    return ret;
}

/**
 * String describing the removal of protected packages.
 */
std::string
Goal::Impl::describeProtectedRemoval()
{
    std::string message(_("The operation would result in removing"
                          " the following protected packages: "));
    Pool * pool = solv->pool;

    if (removalOfProtected && removalOfProtected->size()) {
        Id id = -1;
        std::vector<const char *> names;
        while((id = removalOfProtected->next(id)) != -1) {
            Solvable * s = pool_id2solvable(pool, id);
            names.push_back(pool_id2str(pool, s->name));
        }
        if (names.empty()) {
            return {};
        }
        return message + std::accumulate(std::next(names.begin()), names.end(),
                std::string(names[0]), [](std::string a, std::string b) { return a + ", " + b; });
    }
    auto pset = brokenDependencyAllPkgs(DNF_PACKAGE_STATE_INSTALLED);
    Id id = -1;
    Id protected_kernel = protectedRunningKernel();
    std::vector<const char *> names;
    while((id = pset->next(id)) != -1) {
        if (protectedPkgs->has(id) || id == protected_kernel) {
            Solvable * s = pool_id2solvable(pool, id);
            names.push_back(pool_id2str(pool, s->name));
        }
    }
    if (names.empty())
        return {};
    return message + std::accumulate(std::next(names.begin()), names.end(), std::string(names[0]),
                           [](std::string a, std::string b) { return a + ", " + b; });
}

}
