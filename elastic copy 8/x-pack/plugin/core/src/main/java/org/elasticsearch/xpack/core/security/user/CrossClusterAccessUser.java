/*
 * Copyright Elasticsearch B.V. and/or licensed to Elasticsearch B.V. under one
 * or more contributor license agreements. Licensed under the Elastic License
 * 2.0; you may not use this file except in compliance with the Elastic License
 * 2.0.
 */

package org.elasticsearch.xpack.core.security.user;

import org.elasticsearch.TransportVersion;
import org.elasticsearch.common.Strings;
import org.elasticsearch.xpack.core.security.authc.Authentication;
import org.elasticsearch.xpack.core.security.authc.CrossClusterAccessSubjectInfo;
import org.elasticsearch.xpack.core.security.authz.RoleDescriptor;
import org.elasticsearch.xpack.core.security.authz.RoleDescriptorsIntersection;

import java.io.IOException;
import java.io.UncheckedIOException;

public class CrossClusterAccessUser extends User {
    public static final String NAME = UsernamesField.CROSS_CLUSTER_ACCESS_NAME;

    private static final RoleDescriptor ROLE_DESCRIPTOR = new RoleDescriptor(
        UsernamesField.CROSS_CLUSTER_ACCESS_ROLE,
        new String[] { "cross_cluster_access" },
        null,
        null,
        null,
        null,
        null,
        null,
        null
    );

    public static final User INSTANCE = new CrossClusterAccessUser();

    private CrossClusterAccessUser() {
        super(NAME, Strings.EMPTY_ARRAY);
        // the following traits, and especially the run-as one, go with all the internal users
        // TODO abstract in a base `InternalUser` class
        assert enabled();
        assert roles() != null && roles().length == 0;
    }

    @Override
    public boolean equals(Object o) {
        return INSTANCE == o;
    }

    @Override
    public int hashCode() {
        return System.identityHashCode(this);
    }

    public static boolean is(User user) {
        return INSTANCE.equals(user);
    }

    public static CrossClusterAccessSubjectInfo subjectInfoWithRoleDescriptors(TransportVersion transportVersion, String nodeName) {
        return subjectInfo(transportVersion, nodeName, new RoleDescriptorsIntersection(ROLE_DESCRIPTOR));
    }

    public static CrossClusterAccessSubjectInfo subjectInfoWithEmptyRoleDescriptors(TransportVersion transportVersion, String nodeName) {
        return subjectInfo(transportVersion, nodeName, RoleDescriptorsIntersection.EMPTY);
    }

    private static CrossClusterAccessSubjectInfo subjectInfo(
        TransportVersion transportVersion,
        String nodeName,
        RoleDescriptorsIntersection roleDescriptorsIntersection
    ) {
        try {
            return new CrossClusterAccessSubjectInfo(
                Authentication.newInternalAuthentication(INSTANCE, transportVersion, nodeName),
                roleDescriptorsIntersection
            );
        } catch (IOException e) {
            throw new UncheckedIOException(e);
        }
    }
}
