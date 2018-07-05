// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.models;

import java.util.Date;
import java.util.List;
import java.util.UUID;

import javax.persistence.Column;
import javax.persistence.Entity;
import javax.persistence.EmbeddedId;
import javax.persistence.Id;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import com.avaje.ebean.Model;

import play.data.validation.Constraints;
import play.libs.Json;

@Entity
public class HealthCheck extends Model {
  public static final Logger LOG = LoggerFactory.getLogger(HealthCheck.class);

  // The max number of records to keep per universe.
  public static final int RECORD_LIMIT = 10;

  @EmbeddedId
  @Constraints.Required
  public HealthCheckKey idKey;

  // The customer id, needed only to enforce unique universe names for a customer.
  @Constraints.Required
  public Long customerId;

  // The Json serialized version of the details. This is used only in read from and writing to the
  // DB.
  @Constraints.Required
  @Column(columnDefinition = "TEXT", nullable = false)
  public String detailsJson;

  public static final Find<UUID, HealthCheck> find = new Find<UUID, HealthCheck>() {};

  /**
   * Creates an empty universe.
   * @param taskParams: The details that will describe the universe.
   * @param customerId: UUID of the customer creating the universe
   * @return the newly created universe
   */
  public static HealthCheck addAndPrune(UUID universeUUID, Long customerId, String details) {
    // Create the HealthCheck object.
    HealthCheck check = new HealthCheck();
    check.idKey = HealthCheckKey.create(universeUUID);
    check.customerId = customerId;
    check.detailsJson = details;
    // Save the object.
    check.save();
    keepOnlyLast(universeUUID, RECORD_LIMIT);
    return check;
  }

  public static void keepOnlyLast(UUID universeUUID, int numChecks) {
    List<HealthCheck> checks = getAll(universeUUID);
    for (int i = 0; i < checks.size() - numChecks; ++i) {
      checks.get(i).delete();
    }
  }

  /**
   * Returns the HealthCheck object for a certain universe.
   *
   * @param universeUUID
   * @return the HealthCheck object
   */
  public static List<HealthCheck> getAll(UUID universeUUID) {
    return find.where().eq("universe_uuid", universeUUID).orderBy("check_time").findList();
  }
}