/**
 * Mutable test fixtures must be parsed again after edits. Production trees are
 * immutable. Keep fixture ownership here so boundary tests can edit bytes and
 * explicitly refresh the cached inventory before invoking a consumer.
 */
struct __TreeFixture {
    struct __TreeFixture* Next;
    struct FdtTree*       Tree;
    const void*           Blob;
    size_t                Length;
};

static struct __TreeFixture* g_treeFixtures;

static struct __TreeFixture*
__TreeFixtureFor(struct FdtTree* tree)
{
    struct __TreeFixture* fixture;

    for (fixture = g_treeFixtures; fixture != NULL; fixture = fixture->Next) {
        if (fixture->Tree == tree) {
            return fixture;
        }
    }
    CHECK(0);
    return NULL;
}

static oserr_t
__ParseFixture(const void* blob, size_t length, struct FdtTree** tree)
{
    struct __TreeFixture* fixture;
    oserr_t status = FdtTreeCreate(blob, length, tree);

    if (status != OS_EOK) {
        return status;
    }
    fixture = malloc(sizeof(*fixture));
    CHECK(fixture != NULL);
    *fixture = (struct __TreeFixture) { g_treeFixtures, *tree, blob, length };
    g_treeFixtures = fixture;
    return OS_EOK;
}

static void
__FreeFixtures(void)
{
    struct __TreeFixture* fixture;

    while (g_treeFixtures != NULL) {
        fixture = g_treeFixtures;
        g_treeFixtures = fixture->Next;
        CHECK(atomic_load(&fixture->Tree->References) == 1);
        FdtTreeRelease(fixture->Tree);
        free(fixture);
    }
}

static oserr_t
__RefreshFixture(struct FdtTree* tree)
{
    struct __TreeFixture* fixture = __TreeFixtureFor(tree);
    struct FdtTree* replacement;
    struct FdtTree previous;
    unsigned int references;
    oserr_t status = FdtTreeCreate(fixture->Blob, fixture->Length, &replacement);

    if (status != OS_EOK) {
        return status;
    }
    references = atomic_load(&tree->References);
    previous = *tree;
    *tree = *replacement;
    atomic_store(&tree->References, references);
    *replacement = previous;
    atomic_store(&replacement->References, 1);
    FdtTreeRelease(replacement);
    return OS_EOK;
}

static struct FdtTree*
__RetainFixture(struct FdtTree* tree)
{
    FdtTreeRetain(tree);
    return tree;
}

static oserr_t
__EnumerateFixture(const void* blob, size_t length, FdtPciHostFn callback, void* context)
{
    struct FdtTree* tree;
    oserr_t status = __ParseFixture(blob, length, &tree);

    return status == OS_EOK ? FdtEnumeratePciHosts(tree, callback, context) : status;
}

static oserr_t
__WalkFixture(const void* blob, size_t length, FdtResourceFn callback, void* context)
{
    struct FdtTree* tree;
    oserr_t status = __ParseFixture(blob, length, &tree);

    return status == OS_EOK ? FdtWalkResources(tree, callback, context) : status;
}

static oserr_t
__FixtureChildren(const struct FdtPciHost* host, const struct PciBar* bars,
    FdtRp1DeviceFn callback, void* context)
{
    struct FdtRp1Range apertures[6];
    uint32_t count;
    oserr_t status = __RefreshFixture(host->Tree);

    if (status != OS_EOK) {
        return status;
    }
    status = __Rp1CollectApertures(bars, apertures, &count);
    if (status != OS_EOK) {
        return status;
    }
    return FdtEnumerateRp1Children(host, apertures, count, callback, context);
}

static oserr_t
__FixtureRp1Bus(const struct FdtPciHost* host, const struct PciBar* bars, struct Rp1Bus** bus)
{
    oserr_t status = __RefreshFixture(host->Tree);

    return status == OS_EOK ? Rp1BusCreate(host, bars, bus) : status;
}

static oserr_t
__FixtureRp1Dma(const struct FdtPciHost* host, uint32_t node, struct FdtDmaMap* map)
{
    oserr_t status = __RefreshFixture(host->Tree);

    return status == OS_EOK ? FdtResolveRp1Dma(host, node, map) : status;
}

static oserr_t
__FixtureMsi(const struct FdtPciHost* host, struct FdtPciMsi* msi)
{
    oserr_t status = __RefreshFixture(host->Tree);

    return status == OS_EOK ? FdtResolvePciMsi(host, msi) : status;
}

static oserr_t
__FixtureDependencies(const struct FdtPciHost* host, struct FdtPciDependencies* dependencies)
{
    oserr_t status = __RefreshFixture(host->Tree);

    return status == OS_EOK ? FdtResolvePciDependencies(host, dependencies) : status;
}
